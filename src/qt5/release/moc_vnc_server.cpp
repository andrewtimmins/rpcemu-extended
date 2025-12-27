/****************************************************************************
** Meta object code from reading C++ file 'vnc_server.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../vnc_server.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'vnc_server.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_VncServer_t {
    QByteArrayData data[17];
    char stringdata0[191];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_VncServer_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_VncServer_t qt_meta_stringdata_VncServer = {
    {
QT_MOC_LITERAL(0, 0, 9), // "VncServer"
QT_MOC_LITERAL(1, 10, 15), // "clientConnected"
QT_MOC_LITERAL(2, 26, 0), // ""
QT_MOC_LITERAL(3, 27, 13), // "clientAddress"
QT_MOC_LITERAL(4, 41, 18), // "clientDisconnected"
QT_MOC_LITERAL(5, 60, 13), // "statusChanged"
QT_MOC_LITERAL(6, 74, 7), // "running"
QT_MOC_LITERAL(7, 82, 4), // "port"
QT_MOC_LITERAL(8, 87, 14), // "injectKeyPress"
QT_MOC_LITERAL(9, 102, 8), // "scanCode"
QT_MOC_LITERAL(10, 111, 16), // "injectKeyRelease"
QT_MOC_LITERAL(11, 128, 15), // "injectMouseMove"
QT_MOC_LITERAL(12, 144, 1), // "x"
QT_MOC_LITERAL(13, 146, 1), // "y"
QT_MOC_LITERAL(14, 148, 17), // "injectMouseButton"
QT_MOC_LITERAL(15, 166, 10), // "buttonMask"
QT_MOC_LITERAL(16, 177, 13) // "processEvents"

    },
    "VncServer\0clientConnected\0\0clientAddress\0"
    "clientDisconnected\0statusChanged\0"
    "running\0port\0injectKeyPress\0scanCode\0"
    "injectKeyRelease\0injectMouseMove\0x\0y\0"
    "injectMouseButton\0buttonMask\0processEvents"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_VncServer[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       8,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       7,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   54,    2, 0x06 /* Public */,
       4,    1,   57,    2, 0x06 /* Public */,
       5,    2,   60,    2, 0x06 /* Public */,
       8,    1,   65,    2, 0x06 /* Public */,
      10,    1,   68,    2, 0x06 /* Public */,
      11,    2,   71,    2, 0x06 /* Public */,
      14,    1,   76,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      16,    0,   79,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::Bool, QMetaType::Int,    6,    7,
    QMetaType::Void, QMetaType::UInt,    9,
    QMetaType::Void, QMetaType::UInt,    9,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,   12,   13,
    QMetaType::Void, QMetaType::Int,   15,

 // slots: parameters
    QMetaType::Void,

       0        // eod
};

void VncServer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<VncServer *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->clientConnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->clientDisconnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->statusChanged((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 3: _t->injectKeyPress((*reinterpret_cast< uint(*)>(_a[1]))); break;
        case 4: _t->injectKeyRelease((*reinterpret_cast< uint(*)>(_a[1]))); break;
        case 5: _t->injectMouseMove((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 6: _t->injectMouseButton((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 7: _t->processEvents(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (VncServer::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VncServer::clientConnected)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (VncServer::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VncServer::clientDisconnected)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (VncServer::*)(bool , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VncServer::statusChanged)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (VncServer::*)(unsigned int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VncServer::injectKeyPress)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (VncServer::*)(unsigned int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VncServer::injectKeyRelease)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (VncServer::*)(int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VncServer::injectMouseMove)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (VncServer::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&VncServer::injectMouseButton)) {
                *result = 6;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject VncServer::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_VncServer.data,
    qt_meta_data_VncServer,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *VncServer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *VncServer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_VncServer.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int VncServer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 8)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 8;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 8)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 8;
    }
    return _id;
}

// SIGNAL 0
void VncServer::clientConnected(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void VncServer::clientDisconnected(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void VncServer::statusChanged(bool _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void VncServer::injectKeyPress(unsigned int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void VncServer::injectKeyRelease(unsigned int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void VncServer::injectMouseMove(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void VncServer::injectMouseButton(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
