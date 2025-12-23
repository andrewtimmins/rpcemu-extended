/****************************************************************************
** Meta object code from reading C++ file 'vnc_dialog.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../vnc_dialog.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'vnc_dialog.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_VncDialog_t {
    QByteArrayData data[11];
    char stringdata0[125];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_VncDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_VncDialog_t qt_meta_stringdata_VncDialog = {
    {
QT_MOC_LITERAL(0, 0, 9), // "VncDialog"
QT_MOC_LITERAL(1, 10, 15), // "onEnableToggled"
QT_MOC_LITERAL(2, 26, 0), // ""
QT_MOC_LITERAL(3, 27, 7), // "checked"
QT_MOC_LITERAL(4, 35, 7), // "onApply"
QT_MOC_LITERAL(5, 43, 21), // "onServerStatusChanged"
QT_MOC_LITERAL(6, 65, 7), // "running"
QT_MOC_LITERAL(7, 73, 4), // "port"
QT_MOC_LITERAL(8, 78, 17), // "onClientConnected"
QT_MOC_LITERAL(9, 96, 7), // "address"
QT_MOC_LITERAL(10, 104, 20) // "onClientDisconnected"

    },
    "VncDialog\0onEnableToggled\0\0checked\0"
    "onApply\0onServerStatusChanged\0running\0"
    "port\0onClientConnected\0address\0"
    "onClientDisconnected"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_VncDialog[] = {

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
       1,    1,   39,    2, 0x08 /* Private */,
       4,    0,   42,    2, 0x08 /* Private */,
       5,    2,   43,    2, 0x08 /* Private */,
       8,    1,   48,    2, 0x08 /* Private */,
      10,    1,   51,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void, QMetaType::Bool,    3,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::Int,    6,    7,
    QMetaType::Void, QMetaType::QString,    9,
    QMetaType::Void, QMetaType::QString,    9,

       0        // eod
};

void VncDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<VncDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->onEnableToggled((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 1: _t->onApply(); break;
        case 2: _t->onServerStatusChanged((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 3: _t->onClientConnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->onClientDisconnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject VncDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_VncDialog.data,
    qt_meta_data_VncDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *VncDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *VncDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_VncDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int VncDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
