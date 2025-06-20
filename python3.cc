#include <Python.h>
#include <frameobject.h>
#include <dictobject.h>
#include <longintrepr.h>
#include <unicodeobject.h>
#include <methodobject.h>
#include "libpstack/python.h"

#define DK_SIZE(dk) ((dk)->dk_size)
#define DK_IXSIZE(dk)                \
    (  DK_SIZE(dk) <= 0xff       ? 1 \
    :  DK_SIZE(dk) <= 0xffff     ? 2 \
    :  DK_SIZE(dk) <= 0xffffffff ? 4 \
                                 : sizeof(int64_t))
#define DK_ENTRIES(dk) \
    ((PyDictKeyEntry *)(&((int8_t *)((dk)->dk_indices))[DK_SIZE(dk) * DK_IXSIZE(dk)]))

/*
 * A key of dictionary as implemented by Python3.
 * Applies to versions <= 3.10.x
 */
typedef struct {
    Py_hash_t me_hash;
    PyObject *me_key;
 PyObject *me_value;
} PyDictKeyEntry;

#define DKIX_EMPTY (-1)
#define DKIX_DUMMY (-2)
#define DKIX_ERROR (-3)

/* a type of a lookup function which is used by Python3 dicts internally */
typedef Py_ssize_t (*dict_lookup_func)
    (PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject **value_addr);

/*
 * The dictionary's hashtable as implemented by Python3.
 * Appies to version <= 3.10.x
 */
struct _dictkeysobject {
    Py_ssize_t dk_refcnt;
    Py_ssize_t dk_size;
    dict_lookup_func dk_lookup;
    Py_ssize_t dk_usable;
    Py_ssize_t dk_nentries;
    char dk_indices[];
};

namespace pstack {

template<> std::set<const PythonTypePrinter<3> *> PythonTypePrinter<3>::all = std::set<const PythonTypePrinter<3> *>();
template <>
char PythonTypePrinter<3>::pyBytesType[] = "PyUnicode_Type";

/**
 * @brief Converts a Python PyASCIIObject, PyCompactUnicodeObject or PyUnicodeObjec to a string
 *
 * @param r The reader used
 * @param addr Address of the object
 * @return std::string
 */
template <> std::string readString<3>(const Reader &r, const Elf::Addr addr) {
    PyASCIIObject baseObj = r.readObj<PyASCIIObject>(addr);
    int ascii = baseObj.state.ascii;
    int compact = baseObj.state.compact;
    int ready = baseObj.state.ready;

    if (compact && ascii && ready) {
        return r.readString(addr + sizeof(PyASCIIObject));
    } else if (compact & ready) {
        return r.readString(addr + sizeof(PyCompactUnicodeObject));
    } else {
       return r.readString(addr + offsetof(PyUnicodeObject, data));
    }
}

namespace {

// Reads indexSize bytes at address as a signed int
int64_t readIndex(const Reader &r, const Elf::Addr addr, size_t indexSize) {
    char buf[8];
    r.read(addr, indexSize, buf);
    switch (indexSize) {
        case 1: return *(int8_t *)buf;
        case 2: return *(int16_t *)buf;
        case 4: return *(int32_t *)buf;
        case 8: return *(int64_t *)buf;
        default: throw Exception() << "Envalid dictionary size"; // Shouldn't happen
    }
}
class DictPrinter final : public PythonTypePrinter<3> {
    Elf::Addr print(const PythonPrinter<3> *pc, const PyObject *object, const PyTypeObject *, Elf::Addr) const override {
        PyDictObject *dict = (PyDictObject *)object;
        if (dict->ma_used == 0) {
            pc->os << "{}";
            return 0;
        }

        if (pc->depth > pc->proc.context.options.maxdepth) {
            pc->os << "{ ... }";
            return 0;
        }

        const PyDictKeysObject keys = readPyObj<3, PyDictKeysObject>(*pc->proc.io, Elf::Addr(dict->ma_keys));
        const size_t indexSize = DK_IXSIZE(&keys);
        const Elf::Addr keyEntries = Elf::Addr(dict->ma_keys) + offsetof(PyDictKeysObject, dk_indices) + (keys.dk_size * indexSize);
        const bool isSplit = dict->ma_values != NULL;

        pc->os << "{\n";
        pc->depth++;
        for (Py_ssize_t i = 0; i < keys.dk_size; ++i) {
            auto index = readIndex(*pc->proc.io, Elf::Addr(dict->ma_keys) + offsetof(PyDictKeysObject, dk_indices) + i * indexSize, indexSize);
            if (index == DKIX_EMPTY || index == DKIX_DUMMY) continue;

            PyDictKeyEntry keyEntry = readPyObj<3, PyDictKeyEntry>(*pc->proc.io, keyEntries + index * sizeof(PyDictKeyEntry));

            PyObject* value;
            if (isSplit)
                value = readPyObj<3, PyObject *>(*pc->proc.io, Elf::Addr(dict->ma_values) + index * sizeof(PyObject *));

            pc->os << pc->prefix();
            pc->print(Elf::Addr(keyEntry.me_key));
            pc->os << " : ";
            pc->print(isSplit ? Elf::Addr(value) : Elf::Addr(keyEntry.me_value));
            pc->os << "\n";
        }
        pc->depth--;
        pc->os << pc->prefix() << "}";
        return 0;
    }
    const char *type() const override { return "PyDict_Type"; }
    bool dupdetect() const override { return true; }
};
static DictPrinter dictPrinter;

class BoolPrinter final : public PythonTypePrinter<3> {
    Elf::Addr print(const PythonPrinter<3> *pc, const PyObject *pyo, const PyTypeObject *, Elf::Addr) const override {
        auto pio = (const _longobject *)pyo;
        pc->os << (pio->ob_digit[0] ? "True" : "False");
        return 0;
    }
    const char *type() const override { return "PyBool_Type"; }
    bool dupdetect() const override { return false; }
};
static BoolPrinter boolPrinter;

}

template <typename T, int pyv>  ssize_t
pyRefcnt(const T *o) {
   return o->ob_base.ob_refcnt;
}

template <int pyv, typename T>  const PyTypeObject *
pyObjtype(const T *o) {
   return o->ob_base.ob_type;
}

template <>
int getKwonlyArgCount<3>(const PyObject *pyCode) {
    const PyCodeObject* code = (PyCodeObject *)pyCode;
    return code->co_kwonlyargcount;
}

// this comes from python internals that requires us to include C++-incompatible
// headers (stdatomic.h, for example). It's in its own (C) translation unit.
extern "C" size_t pyInterpOffset();

template <>
std::tuple<Elf::Object::sptr, Elf::Addr, Elf::Addr>
getInterpHead<3>(Procman::Process &proc) {
    Elf::Object::sptr libpython;
    Elf::Addr libpythonAddr;
    Elf::Sym _PyRuntimeSym;

    std::tie(libpython, libpythonAddr, _PyRuntimeSym) = proc.resolveSymbolDetail(
          "_PyRuntime", false,
          [&](std::string_view name) {
             auto base = std::filesystem::path(name).filename();
             return std::string(base).find("python3") != std::string::npos;
          });

    Elf::Addr interpHead = libpythonAddr + _PyRuntimeSym.st_value + pyInterpOffset();

    if (libpython == nullptr) {
        if (proc.context.verbose)
            *proc.context.debug << "Python 3 interpreter not found" << std::endl;
        throw Exception() << "No libpython3 found";
    }

    if (proc.context.verbose)
       *proc.context.debug << "python3 library is " << *libpython->io << std::endl;

    return std::make_tuple(libpython, libpythonAddr, interpHead);
}

}

#include "python.tcc"

namespace pstack {

template struct PythonPrinter<3>;

}
