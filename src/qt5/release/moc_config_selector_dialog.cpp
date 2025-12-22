/****************************************************************************
** Meta object code from reading C++ file 'config_selector_dialog.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../config_selector_dialog.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'config_selector_dialog.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_ConfigSelectorDialog_t {
    QByteArrayData data[11];
    char stringdata0[156];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ConfigSelectorDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ConfigSelectorDialog_t qt_meta_stringdata_ConfigSelectorDialog = {
    {
QT_MOC_LITERAL(0, 0, 20), // "ConfigSelectorDialog"
QT_MOC_LITERAL(1, 21, 12), // "onNewClicked"
QT_MOC_LITERAL(2, 34, 0), // ""
QT_MOC_LITERAL(3, 35, 13), // "onEditClicked"
QT_MOC_LITERAL(4, 49, 15), // "onDeleteClicked"
QT_MOC_LITERAL(5, 65, 14), // "onCloneClicked"
QT_MOC_LITERAL(6, 80, 14), // "onStartClicked"
QT_MOC_LITERAL(7, 95, 19), // "onListDoubleClicked"
QT_MOC_LITERAL(8, 115, 16), // "QListWidgetItem*"
QT_MOC_LITERAL(9, 132, 4), // "item"
QT_MOC_LITERAL(10, 137, 18) // "onSelectionChanged"

    },
    "ConfigSelectorDialog\0onNewClicked\0\0"
    "onEditClicked\0onDeleteClicked\0"
    "onCloneClicked\0onStartClicked\0"
    "onListDoubleClicked\0QListWidgetItem*\0"
    "item\0onSelectionChanged"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ConfigSelectorDialog[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       7,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,   49,    2, 0x08 /* Private */,
       3,    0,   50,    2, 0x08 /* Private */,
       4,    0,   51,    2, 0x08 /* Private */,
       5,    0,   52,    2, 0x08 /* Private */,
       6,    0,   53,    2, 0x08 /* Private */,
       7,    1,   54,    2, 0x08 /* Private */,
      10,    0,   57,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 8,    9,
    QMetaType::Void,

       0        // eod
};

void ConfigSelectorDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ConfigSelectorDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->onNewClicked(); break;
        case 1: _t->onEditClicked(); break;
        case 2: _t->onDeleteClicked(); break;
        case 3: _t->onCloneClicked(); break;
        case 4: _t->onStartClicked(); break;
        case 5: _t->onListDoubleClicked((*reinterpret_cast< QListWidgetItem*(*)>(_a[1]))); break;
        case 6: _t->onSelectionChanged(); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject ConfigSelectorDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_ConfigSelectorDialog.data,
    qt_meta_data_ConfigSelectorDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ConfigSelectorDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ConfigSelectorDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ConfigSelectorDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int ConfigSelectorDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 7;
    }
    return _id;
}
struct qt_meta_stringdata_MachineEditDialog_t {
    QByteArrayData data[8];
    char stringdata0[96];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MachineEditDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MachineEditDialog_t qt_meta_stringdata_MachineEditDialog = {
    {
QT_MOC_LITERAL(0, 0, 17), // "MachineEditDialog"
QT_MOC_LITERAL(1, 18, 11), // "onOkClicked"
QT_MOC_LITERAL(2, 30, 0), // ""
QT_MOC_LITERAL(3, 31, 15), // "onCancelClicked"
QT_MOC_LITERAL(4, 47, 12), // "onHD4Clicked"
QT_MOC_LITERAL(5, 60, 12), // "onHD5Clicked"
QT_MOC_LITERAL(6, 73, 16), // "onNetworkChanged"
QT_MOC_LITERAL(7, 90, 5) // "index"

    },
    "MachineEditDialog\0onOkClicked\0\0"
    "onCancelClicked\0onHD4Clicked\0onHD5Clicked\0"
    "onNetworkChanged\0index"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MachineEditDialog[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,   39,    2, 0x08 /* Private */,
       3,    0,   40,    2, 0x08 /* Private */,
       4,    0,   41,    2, 0x08 /* Private */,
       5,    0,   42,    2, 0x08 /* Private */,
       6,    1,   43,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,    7,

       0        // eod
};

void MachineEditDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MachineEditDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->onOkClicked(); break;
        case 1: _t->onCancelClicked(); break;
        case 2: _t->onHD4Clicked(); break;
        case 3: _t->onHD5Clicked(); break;
        case 4: _t->onNetworkChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject MachineEditDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_MachineEditDialog.data,
    qt_meta_data_MachineEditDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MachineEditDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MachineEditDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MachineEditDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int MachineEditDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
