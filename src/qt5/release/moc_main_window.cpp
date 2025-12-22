/****************************************************************************
** Meta object code from reading C++ file 'main_window.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../main_window.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'main_window.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_MainDisplay_t {
    QByteArrayData data[1];
    char stringdata0[12];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MainDisplay_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MainDisplay_t qt_meta_stringdata_MainDisplay = {
    {
QT_MOC_LITERAL(0, 0, 11) // "MainDisplay"

    },
    "MainDisplay"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MainDisplay[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       0,    0, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

       0        // eod
};

void MainDisplay::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    (void)_o;
    (void)_id;
    (void)_c;
    (void)_a;
}

QT_INIT_METAOBJECT const QMetaObject MainDisplay::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_MainDisplay.data,
    qt_meta_data_MainDisplay,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MainDisplay::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MainDisplay::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MainDisplay.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int MainDisplay::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    return _id;
}
struct qt_meta_stringdata_MainWindow_t {
    QByteArrayData data[48];
    char stringdata0[769];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MainWindow_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MainWindow_t qt_meta_stringdata_MainWindow = {
    {
QT_MOC_LITERAL(0, 0, 10), // "MainWindow"
QT_MOC_LITERAL(1, 11, 19), // "main_display_signal"
QT_MOC_LITERAL(2, 31, 0), // ""
QT_MOC_LITERAL(3, 32, 11), // "VideoUpdate"
QT_MOC_LITERAL(4, 44, 12), // "video_update"
QT_MOC_LITERAL(5, 57, 22), // "move_host_mouse_signal"
QT_MOC_LITERAL(6, 80, 15), // "MouseMoveUpdate"
QT_MOC_LITERAL(7, 96, 12), // "mouse_update"
QT_MOC_LITERAL(8, 109, 27), // "send_nat_rule_to_gui_signal"
QT_MOC_LITERAL(9, 137, 15), // "PortForwardRule"
QT_MOC_LITERAL(10, 153, 4), // "rule"
QT_MOC_LITERAL(11, 158, 12), // "error_signal"
QT_MOC_LITERAL(12, 171, 5), // "error"
QT_MOC_LITERAL(13, 177, 12), // "fatal_signal"
QT_MOC_LITERAL(14, 190, 15), // "menu_screenshot"
QT_MOC_LITERAL(15, 206, 10), // "menu_reset"
QT_MOC_LITERAL(16, 217, 14), // "menu_loaddisc0"
QT_MOC_LITERAL(17, 232, 14), // "menu_loaddisc1"
QT_MOC_LITERAL(18, 247, 17), // "menu_create_disc0"
QT_MOC_LITERAL(19, 265, 17), // "menu_create_disc1"
QT_MOC_LITERAL(20, 283, 19), // "menu_cdrom_disabled"
QT_MOC_LITERAL(21, 303, 16), // "menu_cdrom_empty"
QT_MOC_LITERAL(22, 320, 14), // "menu_cdrom_iso"
QT_MOC_LITERAL(23, 335, 16), // "menu_cdrom_ioctl"
QT_MOC_LITERAL(24, 352, 20), // "menu_cdrom_win_ioctl"
QT_MOC_LITERAL(25, 373, 14), // "menu_configure"
QT_MOC_LITERAL(26, 388, 13), // "menu_nat_list"
QT_MOC_LITERAL(27, 402, 15), // "menu_fullscreen"
QT_MOC_LITERAL(28, 418, 13), // "menu_cpu_idle"
QT_MOC_LITERAL(29, 432, 15), // "menu_mouse_hack"
QT_MOC_LITERAL(30, 448, 20), // "menu_mouse_twobutton"
QT_MOC_LITERAL(31, 469, 18), // "menu_online_manual"
QT_MOC_LITERAL(32, 488, 18), // "menu_visit_website"
QT_MOC_LITERAL(33, 507, 10), // "menu_about"
QT_MOC_LITERAL(34, 518, 22), // "menu_machine_inspector"
QT_MOC_LITERAL(35, 541, 14), // "menu_debug_run"
QT_MOC_LITERAL(36, 556, 16), // "menu_debug_pause"
QT_MOC_LITERAL(37, 573, 15), // "menu_debug_step"
QT_MOC_LITERAL(38, 589, 16), // "menu_debug_step5"
QT_MOC_LITERAL(39, 606, 16), // "menu_aboutToShow"
QT_MOC_LITERAL(40, 623, 16), // "menu_aboutToHide"
QT_MOC_LITERAL(41, 640, 19), // "main_display_update"
QT_MOC_LITERAL(42, 660, 15), // "move_host_mouse"
QT_MOC_LITERAL(43, 676, 20), // "send_nat_rule_to_gui"
QT_MOC_LITERAL(44, 697, 18), // "mips_timer_timeout"
QT_MOC_LITERAL(45, 716, 25), // "application_state_changed"
QT_MOC_LITERAL(46, 742, 20), // "Qt::ApplicationState"
QT_MOC_LITERAL(47, 763, 5) // "state"

    },
    "MainWindow\0main_display_signal\0\0"
    "VideoUpdate\0video_update\0"
    "move_host_mouse_signal\0MouseMoveUpdate\0"
    "mouse_update\0send_nat_rule_to_gui_signal\0"
    "PortForwardRule\0rule\0error_signal\0"
    "error\0fatal_signal\0menu_screenshot\0"
    "menu_reset\0menu_loaddisc0\0menu_loaddisc1\0"
    "menu_create_disc0\0menu_create_disc1\0"
    "menu_cdrom_disabled\0menu_cdrom_empty\0"
    "menu_cdrom_iso\0menu_cdrom_ioctl\0"
    "menu_cdrom_win_ioctl\0menu_configure\0"
    "menu_nat_list\0menu_fullscreen\0"
    "menu_cpu_idle\0menu_mouse_hack\0"
    "menu_mouse_twobutton\0menu_online_manual\0"
    "menu_visit_website\0menu_about\0"
    "menu_machine_inspector\0menu_debug_run\0"
    "menu_debug_pause\0menu_debug_step\0"
    "menu_debug_step5\0menu_aboutToShow\0"
    "menu_aboutToHide\0main_display_update\0"
    "move_host_mouse\0send_nat_rule_to_gui\0"
    "mips_timer_timeout\0application_state_changed\0"
    "Qt::ApplicationState\0state"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MainWindow[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      37,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       5,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,  199,    2, 0x06 /* Public */,
       5,    1,  202,    2, 0x06 /* Public */,
       8,    1,  205,    2, 0x06 /* Public */,
      11,    1,  208,    2, 0x06 /* Public */,
      13,    1,  211,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      14,    0,  214,    2, 0x08 /* Private */,
      15,    0,  215,    2, 0x08 /* Private */,
      16,    0,  216,    2, 0x08 /* Private */,
      17,    0,  217,    2, 0x08 /* Private */,
      18,    0,  218,    2, 0x08 /* Private */,
      19,    0,  219,    2, 0x08 /* Private */,
      20,    0,  220,    2, 0x08 /* Private */,
      21,    0,  221,    2, 0x08 /* Private */,
      22,    0,  222,    2, 0x08 /* Private */,
      23,    0,  223,    2, 0x08 /* Private */,
      24,    0,  224,    2, 0x08 /* Private */,
      25,    0,  225,    2, 0x08 /* Private */,
      26,    0,  226,    2, 0x08 /* Private */,
      27,    0,  227,    2, 0x08 /* Private */,
      28,    0,  228,    2, 0x08 /* Private */,
      29,    0,  229,    2, 0x08 /* Private */,
      30,    0,  230,    2, 0x08 /* Private */,
      31,    0,  231,    2, 0x08 /* Private */,
      32,    0,  232,    2, 0x08 /* Private */,
      33,    0,  233,    2, 0x08 /* Private */,
      34,    0,  234,    2, 0x08 /* Private */,
      35,    0,  235,    2, 0x08 /* Private */,
      36,    0,  236,    2, 0x08 /* Private */,
      37,    0,  237,    2, 0x08 /* Private */,
      38,    0,  238,    2, 0x08 /* Private */,
      39,    0,  239,    2, 0x08 /* Private */,
      40,    0,  240,    2, 0x08 /* Private */,
      41,    1,  241,    2, 0x08 /* Private */,
      42,    1,  244,    2, 0x08 /* Private */,
      43,    1,  247,    2, 0x08 /* Private */,
      44,    0,  250,    2, 0x08 /* Private */,
      45,    1,  251,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 6,    7,
    QMetaType::Void, 0x80000000 | 9,   10,
    QMetaType::Void, QMetaType::QString,   12,
    QMetaType::Void, QMetaType::QString,   12,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 6,    7,
    QMetaType::Void, 0x80000000 | 9,   10,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 46,   47,

       0        // eod
};

void MainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MainWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->main_display_signal((*reinterpret_cast< VideoUpdate(*)>(_a[1]))); break;
        case 1: _t->move_host_mouse_signal((*reinterpret_cast< MouseMoveUpdate(*)>(_a[1]))); break;
        case 2: _t->send_nat_rule_to_gui_signal((*reinterpret_cast< PortForwardRule(*)>(_a[1]))); break;
        case 3: _t->error_signal((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 4: _t->fatal_signal((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 5: _t->menu_screenshot(); break;
        case 6: _t->menu_reset(); break;
        case 7: _t->menu_loaddisc0(); break;
        case 8: _t->menu_loaddisc1(); break;
        case 9: _t->menu_create_disc0(); break;
        case 10: _t->menu_create_disc1(); break;
        case 11: _t->menu_cdrom_disabled(); break;
        case 12: _t->menu_cdrom_empty(); break;
        case 13: _t->menu_cdrom_iso(); break;
        case 14: _t->menu_cdrom_ioctl(); break;
        case 15: _t->menu_cdrom_win_ioctl(); break;
        case 16: _t->menu_configure(); break;
        case 17: _t->menu_nat_list(); break;
        case 18: _t->menu_fullscreen(); break;
        case 19: _t->menu_cpu_idle(); break;
        case 20: _t->menu_mouse_hack(); break;
        case 21: _t->menu_mouse_twobutton(); break;
        case 22: _t->menu_online_manual(); break;
        case 23: _t->menu_visit_website(); break;
        case 24: _t->menu_about(); break;
        case 25: _t->menu_machine_inspector(); break;
        case 26: _t->menu_debug_run(); break;
        case 27: _t->menu_debug_pause(); break;
        case 28: _t->menu_debug_step(); break;
        case 29: _t->menu_debug_step5(); break;
        case 30: _t->menu_aboutToShow(); break;
        case 31: _t->menu_aboutToHide(); break;
        case 32: _t->main_display_update((*reinterpret_cast< VideoUpdate(*)>(_a[1]))); break;
        case 33: _t->move_host_mouse((*reinterpret_cast< MouseMoveUpdate(*)>(_a[1]))); break;
        case 34: _t->send_nat_rule_to_gui((*reinterpret_cast< PortForwardRule(*)>(_a[1]))); break;
        case 35: _t->mips_timer_timeout(); break;
        case 36: _t->application_state_changed((*reinterpret_cast< Qt::ApplicationState(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (MainWindow::*)(VideoUpdate );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MainWindow::main_display_signal)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (MainWindow::*)(MouseMoveUpdate );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MainWindow::move_host_mouse_signal)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (MainWindow::*)(PortForwardRule );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MainWindow::send_nat_rule_to_gui_signal)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (MainWindow::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MainWindow::error_signal)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (MainWindow::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&MainWindow::fatal_signal)) {
                *result = 4;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject MainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_MainWindow.data,
    qt_meta_data_MainWindow,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MainWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int MainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 37)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 37;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 37)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 37;
    }
    return _id;
}

// SIGNAL 0
void MainWindow::main_display_signal(VideoUpdate _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void MainWindow::move_host_mouse_signal(MouseMoveUpdate _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void MainWindow::send_nat_rule_to_gui_signal(PortForwardRule _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void MainWindow::error_signal(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void MainWindow::fatal_signal(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
