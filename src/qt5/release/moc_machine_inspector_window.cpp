/****************************************************************************
** Meta object code from reading C++ file 'machine_inspector_window.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../machine_inspector_window.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'machine_inspector_window.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_MachineInspectorWindow_t {
    QByteArrayData data[28];
    char stringdata0[428];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MachineInspectorWindow_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MachineInspectorWindow_t qt_meta_stringdata_MachineInspectorWindow = {
    {
QT_MOC_LITERAL(0, 0, 22), // "MachineInspectorWindow"
QT_MOC_LITERAL(1, 23, 15), // "refreshSnapshot"
QT_MOC_LITERAL(2, 39, 0), // ""
QT_MOC_LITERAL(3, 40, 14), // "setAutoRefresh"
QT_MOC_LITERAL(4, 55, 7), // "enabled"
QT_MOC_LITERAL(5, 63, 12), // "onRunClicked"
QT_MOC_LITERAL(6, 76, 14), // "onPauseClicked"
QT_MOC_LITERAL(7, 91, 13), // "onStepClicked"
QT_MOC_LITERAL(8, 105, 17), // "onStepFiveClicked"
QT_MOC_LITERAL(9, 123, 15), // "onAddBreakpoint"
QT_MOC_LITERAL(10, 139, 18), // "onRemoveBreakpoint"
QT_MOC_LITERAL(11, 158, 15), // "onAddWatchpoint"
QT_MOC_LITERAL(12, 174, 18), // "onRemoveWatchpoint"
QT_MOC_LITERAL(13, 193, 19), // "onDisasmGoToAddress"
QT_MOC_LITERAL(14, 213, 16), // "onDisasmFollowPC"
QT_MOC_LITERAL(15, 230, 7), // "checked"
QT_MOC_LITERAL(16, 238, 19), // "onMemoryGoToAddress"
QT_MOC_LITERAL(17, 258, 15), // "onMemoryRefresh"
QT_MOC_LITERAL(18, 274, 16), // "onMemoryPrevPage"
QT_MOC_LITERAL(19, 291, 16), // "onMemoryNextPage"
QT_MOC_LITERAL(20, 308, 23), // "onMemoryWordSizeChanged"
QT_MOC_LITERAL(21, 332, 5), // "index"
QT_MOC_LITERAL(22, 338, 15), // "onMemoryJumpROM"
QT_MOC_LITERAL(23, 354, 15), // "onMemoryJumpRAM"
QT_MOC_LITERAL(24, 370, 14), // "onMemoryJumpSP"
QT_MOC_LITERAL(25, 385, 14), // "onMemoryJumpPC"
QT_MOC_LITERAL(26, 400, 14), // "onMemorySearch"
QT_MOC_LITERAL(27, 415, 12) // "onMemoryCopy"

    },
    "MachineInspectorWindow\0refreshSnapshot\0"
    "\0setAutoRefresh\0enabled\0onRunClicked\0"
    "onPauseClicked\0onStepClicked\0"
    "onStepFiveClicked\0onAddBreakpoint\0"
    "onRemoveBreakpoint\0onAddWatchpoint\0"
    "onRemoveWatchpoint\0onDisasmGoToAddress\0"
    "onDisasmFollowPC\0checked\0onMemoryGoToAddress\0"
    "onMemoryRefresh\0onMemoryPrevPage\0"
    "onMemoryNextPage\0onMemoryWordSizeChanged\0"
    "index\0onMemoryJumpROM\0onMemoryJumpRAM\0"
    "onMemoryJumpSP\0onMemoryJumpPC\0"
    "onMemorySearch\0onMemoryCopy"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MachineInspectorWindow[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      23,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,  129,    2, 0x0a /* Public */,
       3,    1,  130,    2, 0x0a /* Public */,
       5,    0,  133,    2, 0x0a /* Public */,
       6,    0,  134,    2, 0x0a /* Public */,
       7,    0,  135,    2, 0x0a /* Public */,
       8,    0,  136,    2, 0x0a /* Public */,
       9,    0,  137,    2, 0x0a /* Public */,
      10,    0,  138,    2, 0x0a /* Public */,
      11,    0,  139,    2, 0x0a /* Public */,
      12,    0,  140,    2, 0x0a /* Public */,
      13,    0,  141,    2, 0x0a /* Public */,
      14,    1,  142,    2, 0x0a /* Public */,
      16,    0,  145,    2, 0x0a /* Public */,
      17,    0,  146,    2, 0x0a /* Public */,
      18,    0,  147,    2, 0x0a /* Public */,
      19,    0,  148,    2, 0x0a /* Public */,
      20,    1,  149,    2, 0x0a /* Public */,
      22,    0,  152,    2, 0x0a /* Public */,
      23,    0,  153,    2, 0x0a /* Public */,
      24,    0,  154,    2, 0x0a /* Public */,
      25,    0,  155,    2, 0x0a /* Public */,
      26,    0,  156,    2, 0x0a /* Public */,
      27,    0,  157,    2, 0x0a /* Public */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,    4,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   15,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   21,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void MachineInspectorWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MachineInspectorWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->refreshSnapshot(); break;
        case 1: _t->setAutoRefresh((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 2: _t->onRunClicked(); break;
        case 3: _t->onPauseClicked(); break;
        case 4: _t->onStepClicked(); break;
        case 5: _t->onStepFiveClicked(); break;
        case 6: _t->onAddBreakpoint(); break;
        case 7: _t->onRemoveBreakpoint(); break;
        case 8: _t->onAddWatchpoint(); break;
        case 9: _t->onRemoveWatchpoint(); break;
        case 10: _t->onDisasmGoToAddress(); break;
        case 11: _t->onDisasmFollowPC((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 12: _t->onMemoryGoToAddress(); break;
        case 13: _t->onMemoryRefresh(); break;
        case 14: _t->onMemoryPrevPage(); break;
        case 15: _t->onMemoryNextPage(); break;
        case 16: _t->onMemoryWordSizeChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 17: _t->onMemoryJumpROM(); break;
        case 18: _t->onMemoryJumpRAM(); break;
        case 19: _t->onMemoryJumpSP(); break;
        case 20: _t->onMemoryJumpPC(); break;
        case 21: _t->onMemorySearch(); break;
        case 22: _t->onMemoryCopy(); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject MachineInspectorWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_MachineInspectorWindow.data,
    qt_meta_data_MachineInspectorWindow,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MachineInspectorWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MachineInspectorWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MachineInspectorWindow.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int MachineInspectorWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 23)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 23;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 23)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 23;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
