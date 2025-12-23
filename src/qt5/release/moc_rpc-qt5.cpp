/****************************************************************************
** Meta object code from reading C++ file 'rpc-qt5.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../rpc-qt5.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'rpc-qt5.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_Emulator_t {
    QByteArrayData data[108];
    char stringdata0[1630];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_Emulator_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_Emulator_t qt_meta_stringdata_Emulator = {
    {
QT_MOC_LITERAL(0, 0, 8), // "Emulator"
QT_MOC_LITERAL(1, 9, 8), // "finished"
QT_MOC_LITERAL(2, 18, 0), // ""
QT_MOC_LITERAL(3, 19, 20), // "video_flyback_signal"
QT_MOC_LITERAL(4, 40, 16), // "key_press_signal"
QT_MOC_LITERAL(5, 57, 9), // "scan_code"
QT_MOC_LITERAL(6, 67, 18), // "key_release_signal"
QT_MOC_LITERAL(7, 86, 17), // "mouse_move_signal"
QT_MOC_LITERAL(8, 104, 1), // "x"
QT_MOC_LITERAL(9, 106, 1), // "y"
QT_MOC_LITERAL(10, 108, 26), // "mouse_move_relative_signal"
QT_MOC_LITERAL(11, 135, 2), // "dx"
QT_MOC_LITERAL(12, 138, 2), // "dy"
QT_MOC_LITERAL(13, 141, 18), // "mouse_press_signal"
QT_MOC_LITERAL(14, 160, 7), // "buttons"
QT_MOC_LITERAL(15, 168, 20), // "mouse_release_signal"
QT_MOC_LITERAL(16, 189, 18), // "mouse_wheel_signal"
QT_MOC_LITERAL(17, 208, 12), // "reset_signal"
QT_MOC_LITERAL(18, 221, 11), // "exit_signal"
QT_MOC_LITERAL(19, 233, 18), // "load_disc_0_signal"
QT_MOC_LITERAL(20, 252, 8), // "discname"
QT_MOC_LITERAL(21, 261, 18), // "load_disc_1_signal"
QT_MOC_LITERAL(22, 280, 19), // "eject_disc_0_signal"
QT_MOC_LITERAL(23, 300, 19), // "eject_disc_1_signal"
QT_MOC_LITERAL(24, 320, 15), // "cpu_idle_signal"
QT_MOC_LITERAL(25, 336, 22), // "integer_scaling_signal"
QT_MOC_LITERAL(26, 359, 21), // "cdrom_disabled_signal"
QT_MOC_LITERAL(27, 381, 18), // "cdrom_empty_signal"
QT_MOC_LITERAL(28, 400, 21), // "cdrom_load_iso_signal"
QT_MOC_LITERAL(29, 422, 18), // "cdrom_ioctl_signal"
QT_MOC_LITERAL(30, 441, 22), // "cdrom_win_ioctl_signal"
QT_MOC_LITERAL(31, 464, 12), // "drive_letter"
QT_MOC_LITERAL(32, 477, 17), // "mouse_hack_signal"
QT_MOC_LITERAL(33, 495, 22), // "mouse_twobutton_signal"
QT_MOC_LITERAL(34, 518, 21), // "config_updated_signal"
QT_MOC_LITERAL(35, 540, 7), // "Config*"
QT_MOC_LITERAL(36, 548, 10), // "new_config"
QT_MOC_LITERAL(37, 559, 5), // "Model"
QT_MOC_LITERAL(38, 565, 9), // "new_model"
QT_MOC_LITERAL(39, 575, 29), // "network_config_updated_signal"
QT_MOC_LITERAL(40, 605, 11), // "NetworkType"
QT_MOC_LITERAL(41, 617, 12), // "network_type"
QT_MOC_LITERAL(42, 630, 10), // "bridgename"
QT_MOC_LITERAL(43, 641, 9), // "ipaddress"
QT_MOC_LITERAL(44, 651, 34), // "show_fullscreen_message_off_s..."
QT_MOC_LITERAL(45, 686, 21), // "switch_machine_signal"
QT_MOC_LITERAL(46, 708, 11), // "config_path"
QT_MOC_LITERAL(47, 720, 23), // "machine_switched_signal"
QT_MOC_LITERAL(48, 744, 12), // "machine_name"
QT_MOC_LITERAL(49, 757, 19), // "nat_rule_add_signal"
QT_MOC_LITERAL(50, 777, 15), // "PortForwardRule"
QT_MOC_LITERAL(51, 793, 4), // "rule"
QT_MOC_LITERAL(52, 798, 20), // "nat_rule_edit_signal"
QT_MOC_LITERAL(53, 819, 8), // "old_rule"
QT_MOC_LITERAL(54, 828, 8), // "new_rule"
QT_MOC_LITERAL(55, 837, 22), // "nat_rule_remove_signal"
QT_MOC_LITERAL(56, 860, 29), // "debugger_state_changed_signal"
QT_MOC_LITERAL(57, 890, 11), // "mainemuloop"
QT_MOC_LITERAL(58, 902, 13), // "video_flyback"
QT_MOC_LITERAL(59, 916, 9), // "key_press"
QT_MOC_LITERAL(60, 926, 11), // "key_release"
QT_MOC_LITERAL(61, 938, 10), // "mouse_move"
QT_MOC_LITERAL(62, 949, 19), // "mouse_move_relative"
QT_MOC_LITERAL(63, 969, 11), // "mouse_press"
QT_MOC_LITERAL(64, 981, 13), // "mouse_release"
QT_MOC_LITERAL(65, 995, 11), // "mouse_wheel"
QT_MOC_LITERAL(66, 1007, 5), // "reset"
QT_MOC_LITERAL(67, 1013, 4), // "exit"
QT_MOC_LITERAL(68, 1018, 11), // "load_disc_0"
QT_MOC_LITERAL(69, 1030, 11), // "load_disc_1"
QT_MOC_LITERAL(70, 1042, 12), // "eject_disc_0"
QT_MOC_LITERAL(71, 1055, 12), // "eject_disc_1"
QT_MOC_LITERAL(72, 1068, 14), // "switch_machine"
QT_MOC_LITERAL(73, 1083, 8), // "cpu_idle"
QT_MOC_LITERAL(74, 1092, 15), // "integer_scaling"
QT_MOC_LITERAL(75, 1108, 14), // "cdrom_disabled"
QT_MOC_LITERAL(76, 1123, 11), // "cdrom_empty"
QT_MOC_LITERAL(77, 1135, 14), // "cdrom_load_iso"
QT_MOC_LITERAL(78, 1150, 11), // "cdrom_ioctl"
QT_MOC_LITERAL(79, 1162, 10), // "mouse_hack"
QT_MOC_LITERAL(80, 1173, 15), // "mouse_twobutton"
QT_MOC_LITERAL(81, 1189, 14), // "config_updated"
QT_MOC_LITERAL(82, 1204, 22), // "network_config_updated"
QT_MOC_LITERAL(83, 1227, 27), // "show_fullscreen_message_off"
QT_MOC_LITERAL(84, 1255, 12), // "nat_rule_add"
QT_MOC_LITERAL(85, 1268, 13), // "nat_rule_edit"
QT_MOC_LITERAL(86, 1282, 15), // "nat_rule_remove"
QT_MOC_LITERAL(87, 1298, 14), // "debugger_pause"
QT_MOC_LITERAL(88, 1313, 15), // "debugger_resume"
QT_MOC_LITERAL(89, 1329, 13), // "debugger_step"
QT_MOC_LITERAL(90, 1343, 15), // "debugger_step_n"
QT_MOC_LITERAL(91, 1359, 17), // "instruction_count"
QT_MOC_LITERAL(92, 1377, 23), // "debugger_add_breakpoint"
QT_MOC_LITERAL(93, 1401, 7), // "address"
QT_MOC_LITERAL(94, 1409, 26), // "debugger_remove_breakpoint"
QT_MOC_LITERAL(95, 1436, 26), // "debugger_clear_breakpoints"
QT_MOC_LITERAL(96, 1463, 23), // "debugger_add_watchpoint"
QT_MOC_LITERAL(97, 1487, 4), // "size"
QT_MOC_LITERAL(98, 1492, 7), // "on_read"
QT_MOC_LITERAL(99, 1500, 8), // "on_write"
QT_MOC_LITERAL(100, 1509, 26), // "debugger_remove_watchpoint"
QT_MOC_LITERAL(101, 1536, 26), // "debugger_clear_watchpoints"
QT_MOC_LITERAL(102, 1563, 12), // "takeSnapshot"
QT_MOC_LITERAL(103, 1576, 15), // "MachineSnapshot"
QT_MOC_LITERAL(104, 1592, 10), // "readMemory"
QT_MOC_LITERAL(105, 1603, 6), // "length"
QT_MOC_LITERAL(106, 1610, 13), // "disassembleAt"
QT_MOC_LITERAL(107, 1624, 5) // "count"

    },
    "Emulator\0finished\0\0video_flyback_signal\0"
    "key_press_signal\0scan_code\0"
    "key_release_signal\0mouse_move_signal\0"
    "x\0y\0mouse_move_relative_signal\0dx\0dy\0"
    "mouse_press_signal\0buttons\0"
    "mouse_release_signal\0mouse_wheel_signal\0"
    "reset_signal\0exit_signal\0load_disc_0_signal\0"
    "discname\0load_disc_1_signal\0"
    "eject_disc_0_signal\0eject_disc_1_signal\0"
    "cpu_idle_signal\0integer_scaling_signal\0"
    "cdrom_disabled_signal\0cdrom_empty_signal\0"
    "cdrom_load_iso_signal\0cdrom_ioctl_signal\0"
    "cdrom_win_ioctl_signal\0drive_letter\0"
    "mouse_hack_signal\0mouse_twobutton_signal\0"
    "config_updated_signal\0Config*\0new_config\0"
    "Model\0new_model\0network_config_updated_signal\0"
    "NetworkType\0network_type\0bridgename\0"
    "ipaddress\0show_fullscreen_message_off_signal\0"
    "switch_machine_signal\0config_path\0"
    "machine_switched_signal\0machine_name\0"
    "nat_rule_add_signal\0PortForwardRule\0"
    "rule\0nat_rule_edit_signal\0old_rule\0"
    "new_rule\0nat_rule_remove_signal\0"
    "debugger_state_changed_signal\0mainemuloop\0"
    "video_flyback\0key_press\0key_release\0"
    "mouse_move\0mouse_move_relative\0"
    "mouse_press\0mouse_release\0mouse_wheel\0"
    "reset\0exit\0load_disc_0\0load_disc_1\0"
    "eject_disc_0\0eject_disc_1\0switch_machine\0"
    "cpu_idle\0integer_scaling\0cdrom_disabled\0"
    "cdrom_empty\0cdrom_load_iso\0cdrom_ioctl\0"
    "mouse_hack\0mouse_twobutton\0config_updated\0"
    "network_config_updated\0"
    "show_fullscreen_message_off\0nat_rule_add\0"
    "nat_rule_edit\0nat_rule_remove\0"
    "debugger_pause\0debugger_resume\0"
    "debugger_step\0debugger_step_n\0"
    "instruction_count\0debugger_add_breakpoint\0"
    "address\0debugger_remove_breakpoint\0"
    "debugger_clear_breakpoints\0"
    "debugger_add_watchpoint\0size\0on_read\0"
    "on_write\0debugger_remove_watchpoint\0"
    "debugger_clear_watchpoints\0takeSnapshot\0"
    "MachineSnapshot\0readMemory\0length\0"
    "disassembleAt\0count"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_Emulator[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      76,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      33,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,  394,    2, 0x06 /* Public */,
       3,    0,  395,    2, 0x06 /* Public */,
       4,    1,  396,    2, 0x06 /* Public */,
       6,    1,  399,    2, 0x06 /* Public */,
       7,    2,  402,    2, 0x06 /* Public */,
      10,    2,  407,    2, 0x06 /* Public */,
      13,    1,  412,    2, 0x06 /* Public */,
      15,    1,  415,    2, 0x06 /* Public */,
      16,    1,  418,    2, 0x06 /* Public */,
      17,    0,  421,    2, 0x06 /* Public */,
      18,    0,  422,    2, 0x06 /* Public */,
      19,    1,  423,    2, 0x06 /* Public */,
      21,    1,  426,    2, 0x06 /* Public */,
      22,    0,  429,    2, 0x06 /* Public */,
      23,    0,  430,    2, 0x06 /* Public */,
      24,    0,  431,    2, 0x06 /* Public */,
      25,    0,  432,    2, 0x06 /* Public */,
      26,    0,  433,    2, 0x06 /* Public */,
      27,    0,  434,    2, 0x06 /* Public */,
      28,    1,  435,    2, 0x06 /* Public */,
      29,    0,  438,    2, 0x06 /* Public */,
      30,    1,  439,    2, 0x06 /* Public */,
      32,    0,  442,    2, 0x06 /* Public */,
      33,    0,  443,    2, 0x06 /* Public */,
      34,    2,  444,    2, 0x06 /* Public */,
      39,    3,  449,    2, 0x06 /* Public */,
      44,    0,  456,    2, 0x06 /* Public */,
      45,    1,  457,    2, 0x06 /* Public */,
      47,    1,  460,    2, 0x06 /* Public */,
      49,    1,  463,    2, 0x06 /* Public */,
      52,    2,  466,    2, 0x06 /* Public */,
      55,    1,  471,    2, 0x06 /* Public */,
      56,    0,  474,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      57,    0,  475,    2, 0x0a /* Public */,
      58,    0,  476,    2, 0x0a /* Public */,
      59,    1,  477,    2, 0x0a /* Public */,
      60,    1,  480,    2, 0x0a /* Public */,
      61,    2,  483,    2, 0x0a /* Public */,
      62,    2,  488,    2, 0x0a /* Public */,
      63,    1,  493,    2, 0x0a /* Public */,
      64,    1,  496,    2, 0x0a /* Public */,
      65,    1,  499,    2, 0x0a /* Public */,
      66,    0,  502,    2, 0x0a /* Public */,
      67,    0,  503,    2, 0x0a /* Public */,
      68,    1,  504,    2, 0x0a /* Public */,
      69,    1,  507,    2, 0x0a /* Public */,
      70,    0,  510,    2, 0x0a /* Public */,
      71,    0,  511,    2, 0x0a /* Public */,
      72,    1,  512,    2, 0x0a /* Public */,
      73,    0,  515,    2, 0x0a /* Public */,
      74,    0,  516,    2, 0x0a /* Public */,
      75,    0,  517,    2, 0x0a /* Public */,
      76,    0,  518,    2, 0x0a /* Public */,
      77,    1,  519,    2, 0x0a /* Public */,
      78,    0,  522,    2, 0x0a /* Public */,
      79,    0,  523,    2, 0x0a /* Public */,
      80,    0,  524,    2, 0x0a /* Public */,
      81,    2,  525,    2, 0x0a /* Public */,
      82,    3,  530,    2, 0x0a /* Public */,
      83,    0,  537,    2, 0x0a /* Public */,
      84,    1,  538,    2, 0x0a /* Public */,
      85,    2,  541,    2, 0x0a /* Public */,
      86,    1,  546,    2, 0x0a /* Public */,
      87,    0,  549,    2, 0x0a /* Public */,
      88,    0,  550,    2, 0x0a /* Public */,
      89,    0,  551,    2, 0x0a /* Public */,
      90,    1,  552,    2, 0x0a /* Public */,
      92,    1,  555,    2, 0x0a /* Public */,
      94,    1,  558,    2, 0x0a /* Public */,
      95,    0,  561,    2, 0x0a /* Public */,
      96,    4,  562,    2, 0x0a /* Public */,
     100,    4,  571,    2, 0x0a /* Public */,
     101,    0,  580,    2, 0x0a /* Public */,

 // methods: name, argc, parameters, tag, flags
     102,    0,  581,    2, 0x02 /* Public */,
     104,    2,  582,    2, 0x02 /* Public */,
     106,    2,  587,    2, 0x02 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::UInt,    5,
    QMetaType::Void, QMetaType::UInt,    5,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,    8,    9,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,   11,   12,
    QMetaType::Void, QMetaType::Int,   14,
    QMetaType::Void, QMetaType::Int,   14,
    QMetaType::Void, QMetaType::Int,   12,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Char,   31,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 35, 0x80000000 | 37,   36,   38,
    QMetaType::Void, 0x80000000 | 40, QMetaType::QString, QMetaType::QString,   41,   42,   43,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   46,
    QMetaType::Void, QMetaType::QString,   48,
    QMetaType::Void, 0x80000000 | 50,   51,
    QMetaType::Void, 0x80000000 | 50, 0x80000000 | 50,   53,   54,
    QMetaType::Void, 0x80000000 | 50,   51,
    QMetaType::Void,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::UInt,    5,
    QMetaType::Void, QMetaType::UInt,    5,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,    8,    9,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,   11,   12,
    QMetaType::Void, QMetaType::Int,   14,
    QMetaType::Void, QMetaType::Int,   14,
    QMetaType::Void, QMetaType::Int,   12,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   46,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 35, 0x80000000 | 37,   36,   38,
    QMetaType::Void, 0x80000000 | 40, QMetaType::QString, QMetaType::QString,   41,   42,   43,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 50,   51,
    QMetaType::Void, 0x80000000 | 50, 0x80000000 | 50,   53,   54,
    QMetaType::Void, 0x80000000 | 50,   51,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::UInt,   91,
    QMetaType::Void, QMetaType::UInt,   93,
    QMetaType::Void, QMetaType::UInt,   93,
    QMetaType::Void,
    QMetaType::Void, QMetaType::UInt, QMetaType::UInt, QMetaType::Bool, QMetaType::Bool,   93,   97,   98,   99,
    QMetaType::Void, QMetaType::UInt, QMetaType::UInt, QMetaType::Bool, QMetaType::Bool,   93,   97,   98,   99,
    QMetaType::Void,

 // methods: parameters
    0x80000000 | 103,
    QMetaType::QByteArray, QMetaType::UInt, QMetaType::UInt,   93,  105,
    QMetaType::QString, QMetaType::UInt, QMetaType::Int,   93,  107,

       0        // eod
};

void Emulator::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<Emulator *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->finished(); break;
        case 1: _t->video_flyback_signal(); break;
        case 2: _t->key_press_signal((*reinterpret_cast< uint(*)>(_a[1]))); break;
        case 3: _t->key_release_signal((*reinterpret_cast< uint(*)>(_a[1]))); break;
        case 4: _t->mouse_move_signal((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 5: _t->mouse_move_relative_signal((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 6: _t->mouse_press_signal((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 7: _t->mouse_release_signal((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 8: _t->mouse_wheel_signal((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 9: _t->reset_signal(); break;
        case 10: _t->exit_signal(); break;
        case 11: _t->load_disc_0_signal((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 12: _t->load_disc_1_signal((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 13: _t->eject_disc_0_signal(); break;
        case 14: _t->eject_disc_1_signal(); break;
        case 15: _t->cpu_idle_signal(); break;
        case 16: _t->integer_scaling_signal(); break;
        case 17: _t->cdrom_disabled_signal(); break;
        case 18: _t->cdrom_empty_signal(); break;
        case 19: _t->cdrom_load_iso_signal((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 20: _t->cdrom_ioctl_signal(); break;
        case 21: _t->cdrom_win_ioctl_signal((*reinterpret_cast< char(*)>(_a[1]))); break;
        case 22: _t->mouse_hack_signal(); break;
        case 23: _t->mouse_twobutton_signal(); break;
        case 24: _t->config_updated_signal((*reinterpret_cast< Config*(*)>(_a[1])),(*reinterpret_cast< Model(*)>(_a[2]))); break;
        case 25: _t->network_config_updated_signal((*reinterpret_cast< NetworkType(*)>(_a[1])),(*reinterpret_cast< QString(*)>(_a[2])),(*reinterpret_cast< QString(*)>(_a[3]))); break;
        case 26: _t->show_fullscreen_message_off_signal(); break;
        case 27: _t->switch_machine_signal((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 28: _t->machine_switched_signal((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 29: _t->nat_rule_add_signal((*reinterpret_cast< PortForwardRule(*)>(_a[1]))); break;
        case 30: _t->nat_rule_edit_signal((*reinterpret_cast< PortForwardRule(*)>(_a[1])),(*reinterpret_cast< PortForwardRule(*)>(_a[2]))); break;
        case 31: _t->nat_rule_remove_signal((*reinterpret_cast< PortForwardRule(*)>(_a[1]))); break;
        case 32: _t->debugger_state_changed_signal(); break;
        case 33: _t->mainemuloop(); break;
        case 34: _t->video_flyback(); break;
        case 35: _t->key_press((*reinterpret_cast< uint(*)>(_a[1]))); break;
        case 36: _t->key_release((*reinterpret_cast< uint(*)>(_a[1]))); break;
        case 37: _t->mouse_move((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 38: _t->mouse_move_relative((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 39: _t->mouse_press((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 40: _t->mouse_release((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 41: _t->mouse_wheel((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 42: _t->reset(); break;
        case 43: _t->exit(); break;
        case 44: _t->load_disc_0((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 45: _t->load_disc_1((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 46: _t->eject_disc_0(); break;
        case 47: _t->eject_disc_1(); break;
        case 48: _t->switch_machine((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 49: _t->cpu_idle(); break;
        case 50: _t->integer_scaling(); break;
        case 51: _t->cdrom_disabled(); break;
        case 52: _t->cdrom_empty(); break;
        case 53: _t->cdrom_load_iso((*reinterpret_cast< QString(*)>(_a[1]))); break;
        case 54: _t->cdrom_ioctl(); break;
        case 55: _t->mouse_hack(); break;
        case 56: _t->mouse_twobutton(); break;
        case 57: _t->config_updated((*reinterpret_cast< Config*(*)>(_a[1])),(*reinterpret_cast< Model(*)>(_a[2]))); break;
        case 58: _t->network_config_updated((*reinterpret_cast< NetworkType(*)>(_a[1])),(*reinterpret_cast< QString(*)>(_a[2])),(*reinterpret_cast< QString(*)>(_a[3]))); break;
        case 59: _t->show_fullscreen_message_off(); break;
        case 60: _t->nat_rule_add((*reinterpret_cast< PortForwardRule(*)>(_a[1]))); break;
        case 61: _t->nat_rule_edit((*reinterpret_cast< PortForwardRule(*)>(_a[1])),(*reinterpret_cast< PortForwardRule(*)>(_a[2]))); break;
        case 62: _t->nat_rule_remove((*reinterpret_cast< PortForwardRule(*)>(_a[1]))); break;
        case 63: _t->debugger_pause(); break;
        case 64: _t->debugger_resume(); break;
        case 65: _t->debugger_step(); break;
        case 66: _t->debugger_step_n((*reinterpret_cast< quint32(*)>(_a[1]))); break;
        case 67: _t->debugger_add_breakpoint((*reinterpret_cast< quint32(*)>(_a[1]))); break;
        case 68: _t->debugger_remove_breakpoint((*reinterpret_cast< quint32(*)>(_a[1]))); break;
        case 69: _t->debugger_clear_breakpoints(); break;
        case 70: _t->debugger_add_watchpoint((*reinterpret_cast< quint32(*)>(_a[1])),(*reinterpret_cast< quint32(*)>(_a[2])),(*reinterpret_cast< bool(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4]))); break;
        case 71: _t->debugger_remove_watchpoint((*reinterpret_cast< quint32(*)>(_a[1])),(*reinterpret_cast< quint32(*)>(_a[2])),(*reinterpret_cast< bool(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4]))); break;
        case 72: _t->debugger_clear_watchpoints(); break;
        case 73: { MachineSnapshot _r = _t->takeSnapshot();
            if (_a[0]) *reinterpret_cast< MachineSnapshot*>(_a[0]) = std::move(_r); }  break;
        case 74: { QByteArray _r = _t->readMemory((*reinterpret_cast< quint32(*)>(_a[1])),(*reinterpret_cast< quint32(*)>(_a[2])));
            if (_a[0]) *reinterpret_cast< QByteArray*>(_a[0]) = std::move(_r); }  break;
        case 75: { QString _r = _t->disassembleAt((*reinterpret_cast< quint32(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2])));
            if (_a[0]) *reinterpret_cast< QString*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::finished)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::video_flyback_signal)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(unsigned  );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::key_press_signal)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(unsigned  );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::key_release_signal)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::mouse_move_signal)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::mouse_move_relative_signal)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::mouse_press_signal)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::mouse_release_signal)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::mouse_wheel_signal)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::reset_signal)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::exit_signal)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::load_disc_0_signal)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::load_disc_1_signal)) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::eject_disc_0_signal)) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::eject_disc_1_signal)) {
                *result = 14;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::cpu_idle_signal)) {
                *result = 15;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::integer_scaling_signal)) {
                *result = 16;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::cdrom_disabled_signal)) {
                *result = 17;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::cdrom_empty_signal)) {
                *result = 18;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::cdrom_load_iso_signal)) {
                *result = 19;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::cdrom_ioctl_signal)) {
                *result = 20;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(char );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::cdrom_win_ioctl_signal)) {
                *result = 21;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::mouse_hack_signal)) {
                *result = 22;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::mouse_twobutton_signal)) {
                *result = 23;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(Config * , Model );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::config_updated_signal)) {
                *result = 24;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(NetworkType , QString , QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::network_config_updated_signal)) {
                *result = 25;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::show_fullscreen_message_off_signal)) {
                *result = 26;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::switch_machine_signal)) {
                *result = 27;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(QString );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::machine_switched_signal)) {
                *result = 28;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(PortForwardRule );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::nat_rule_add_signal)) {
                *result = 29;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(PortForwardRule , PortForwardRule );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::nat_rule_edit_signal)) {
                *result = 30;
                return;
            }
        }
        {
            using _t = void (Emulator::*)(PortForwardRule );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::nat_rule_remove_signal)) {
                *result = 31;
                return;
            }
        }
        {
            using _t = void (Emulator::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&Emulator::debugger_state_changed_signal)) {
                *result = 32;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject Emulator::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_Emulator.data,
    qt_meta_data_Emulator,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *Emulator::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *Emulator::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_Emulator.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int Emulator::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 76)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 76;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 76)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 76;
    }
    return _id;
}

// SIGNAL 0
void Emulator::finished()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void Emulator::video_flyback_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void Emulator::key_press_signal(unsigned  _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void Emulator::key_release_signal(unsigned  _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void Emulator::mouse_move_signal(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void Emulator::mouse_move_relative_signal(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void Emulator::mouse_press_signal(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void Emulator::mouse_release_signal(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void Emulator::mouse_wheel_signal(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void Emulator::reset_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 9, nullptr);
}

// SIGNAL 10
void Emulator::exit_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void Emulator::load_disc_0_signal(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void Emulator::load_disc_1_signal(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void Emulator::eject_disc_0_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 13, nullptr);
}

// SIGNAL 14
void Emulator::eject_disc_1_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 14, nullptr);
}

// SIGNAL 15
void Emulator::cpu_idle_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 15, nullptr);
}

// SIGNAL 16
void Emulator::integer_scaling_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 16, nullptr);
}

// SIGNAL 17
void Emulator::cdrom_disabled_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 17, nullptr);
}

// SIGNAL 18
void Emulator::cdrom_empty_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 18, nullptr);
}

// SIGNAL 19
void Emulator::cdrom_load_iso_signal(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 19, _a);
}

// SIGNAL 20
void Emulator::cdrom_ioctl_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 20, nullptr);
}

// SIGNAL 21
void Emulator::cdrom_win_ioctl_signal(char _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 21, _a);
}

// SIGNAL 22
void Emulator::mouse_hack_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 22, nullptr);
}

// SIGNAL 23
void Emulator::mouse_twobutton_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 23, nullptr);
}

// SIGNAL 24
void Emulator::config_updated_signal(Config * _t1, Model _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 24, _a);
}

// SIGNAL 25
void Emulator::network_config_updated_signal(NetworkType _t1, QString _t2, QString _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 25, _a);
}

// SIGNAL 26
void Emulator::show_fullscreen_message_off_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 26, nullptr);
}

// SIGNAL 27
void Emulator::switch_machine_signal(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 27, _a);
}

// SIGNAL 28
void Emulator::machine_switched_signal(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 28, _a);
}

// SIGNAL 29
void Emulator::nat_rule_add_signal(PortForwardRule _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 29, _a);
}

// SIGNAL 30
void Emulator::nat_rule_edit_signal(PortForwardRule _t1, PortForwardRule _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 30, _a);
}

// SIGNAL 31
void Emulator::nat_rule_remove_signal(PortForwardRule _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 31, _a);
}

// SIGNAL 32
void Emulator::debugger_state_changed_signal()
{
    QMetaObject::activate(this, &staticMetaObject, 32, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
