/*
  RPCEmu - An Acorn system emulator

  Copyright (C) 2025-2026 Andy Timmins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config_paths.h"

#include <cstring>
#include <strings.h>

#include <wx/fileconf.h>
#include <wx/filename.h>

extern "C" {
#include "rpcemu.h"
#include "peripheral_config.h"
#include "podule_config.h"
}

#include <wx/arrstr.h>

static char current_config_path[512] = "";

static void peripheral_config_load(wxFileConfig &settings)
{
	ConfigFileUseGeneralGroup(settings);
	long mode = 0;
	settings.Read("serial_com1_mode", &mode, 0L);
	peripheral_config.com1_mode = static_cast<PeripheralSerialMode>(mode);
	settings.Read("serial_com2_mode", &mode, 0L);
	peripheral_config.com2_mode = static_cast<PeripheralSerialMode>(mode);
	settings.Read("parallel_mode", &mode, 0L);
	peripheral_config.parallel_mode = static_cast<PeripheralParallelMode>(mode);

	wxString value;
	settings.Read("serial_com1_log", &value, wxEmptyString);
	strncpy(peripheral_config.com1_log_path, value.utf8_str().data(),
	        sizeof(peripheral_config.com1_log_path) - 1);
	settings.Read("serial_com2_log", &value, wxEmptyString);
	strncpy(peripheral_config.com2_log_path, value.utf8_str().data(),
	        sizeof(peripheral_config.com2_log_path) - 1);
	settings.Read("serial_com1_device", &value, wxEmptyString);
	strncpy(peripheral_config.com1_device, value.utf8_str().data(),
	        sizeof(peripheral_config.com1_device) - 1);
	settings.Read("serial_com2_device", &value, wxEmptyString);
	strncpy(peripheral_config.com2_device, value.utf8_str().data(),
	        sizeof(peripheral_config.com2_device) - 1);
	settings.Read("parallel_log", &value, wxEmptyString);
	strncpy(peripheral_config.parallel_log_path, value.utf8_str().data(),
	        sizeof(peripheral_config.parallel_log_path) - 1);
	settings.Read("parallel_device", &value, wxEmptyString);
	strncpy(peripheral_config.parallel_device, value.utf8_str().data(),
	        sizeof(peripheral_config.parallel_device) - 1);
	settings.Read("printer_output_path", &value, wxEmptyString);
	strncpy(peripheral_config.printer_output_path, value.utf8_str().data(),
	        sizeof(peripheral_config.printer_output_path) - 1);

	long auto_pdf = 0;
	settings.Read("printer_auto_pdf", &auto_pdf, 0L);
	peripheral_config.printer_auto_pdf = auto_pdf ? 1 : 0;

	peripheral_config.com1_log_path[sizeof(peripheral_config.com1_log_path) - 1] = '\0';
	peripheral_config.com2_log_path[sizeof(peripheral_config.com2_log_path) - 1] = '\0';
	peripheral_config.com1_device[sizeof(peripheral_config.com1_device) - 1] = '\0';
	peripheral_config.com2_device[sizeof(peripheral_config.com2_device) - 1] = '\0';
	peripheral_config.parallel_log_path[sizeof(peripheral_config.parallel_log_path) - 1] = '\0';
	peripheral_config.parallel_device[sizeof(peripheral_config.parallel_device) - 1] = '\0';
	peripheral_config.printer_output_path[sizeof(peripheral_config.printer_output_path) - 1] = '\0';
}

static void peripheral_config_save(wxFileConfig &settings)
{
	ConfigFileUseGeneralGroup(settings);
	settings.Write("serial_com1_mode", static_cast<long>(peripheral_config.com1_mode));
	settings.Write("serial_com2_mode", static_cast<long>(peripheral_config.com2_mode));
	settings.Write("parallel_mode", static_cast<long>(peripheral_config.parallel_mode));
	settings.Write("serial_com1_log", wxString(peripheral_config.com1_log_path, wxConvUTF8));
	settings.Write("serial_com2_log", wxString(peripheral_config.com2_log_path, wxConvUTF8));
	settings.Write("serial_com1_device", wxString(peripheral_config.com1_device, wxConvUTF8));
	settings.Write("serial_com2_device", wxString(peripheral_config.com2_device, wxConvUTF8));
	settings.Write("parallel_log", wxString(peripheral_config.parallel_log_path, wxConvUTF8));
	settings.Write("parallel_device", wxString(peripheral_config.parallel_device, wxConvUTF8));
	settings.Write("printer_output_path", wxString(peripheral_config.printer_output_path, wxConvUTF8));
	settings.Write("printer_auto_pdf", static_cast<long>(peripheral_config.printer_auto_pdf));
}

static void machine_cmos_sync(const char *machine_name, Model model, unsigned mem_size, unsigned vram_size)
{
	char meta_path[512];
	char cmos_path[512];

	snprintf(meta_path, sizeof(meta_path), "%smachines/%s/emulator.meta",
	         rpcemu_get_datadir(), machine_name);

	wxFileConfig meta(wxEmptyString, wxEmptyString,
	                  wxString::FromUTF8(meta_path), wxEmptyString,
	                  wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(meta);

	wxString old_model;
	meta.Read("model", &old_model, wxEmptyString);
	const wxString new_model = wxString::FromUTF8(models[model].name_config);

	/*
	 * Only a change of machine MODEL warrants discarding the saved CMOS. RISC OS
	 * probes RAM and VRAM sizes at boot, so mem_size/vram_size changes do not
	 * need a reset; wiping the CMOS on any config tweak (as this used to) threw
	 * away the user's RISC OS configuration - boot options, filing system,
	 * screen setup - every time they adjusted memory (issue #28). A model change
	 * is effectively a different machine, whose CMOS defaults can genuinely
	 * differ, so the reset is kept for that case only. A first boot with no
	 * recorded model adopts the existing CMOS rather than clearing it.
	 */
	if (!old_model.empty() && old_model != new_model) {
		snprintf(cmos_path, sizeof(cmos_path), "%scmos.ram", rpcemu_get_machine_datadir());
		if (wxRemoveFile(wxString::FromUTF8(cmos_path))) {
			rpclog("config_load: cleared CMOS after machine model change\n");
		}
	}

	meta.Write("model", new_model);
	meta.Write("mem_size", static_cast<long>(mem_size));
	meta.Write("vram_size", static_cast<long>(vram_size));
	meta.Flush();
}

extern "C" void config_set_path(const char *path)
{
	if (path && strlen(path) < sizeof(current_config_path)) {
		strncpy(current_config_path, path, sizeof(current_config_path) - 1);
		current_config_path[sizeof(current_config_path) - 1] = '\0';
	}
}

extern "C" const char *config_get_path(void)
{
	if (current_config_path[0] != '\0') {
		return current_config_path;
	}
	return "configs/Default.cfg";
}

static void config_nat_rules_load(wxFileConfig &settings)
{
	settings.SetPath("/nat_port_forward_rules");
	long size = 0;
	if (settings.Read("size", &size, 0L) && size > 0) {
		for (long i = 0; i < size && i < MAX_PORT_FORWARDS; ++i) {
			settings.SetPath(wxString::Format("/nat_port_forward_rules/%ld", i));

			wxString rule_type_name;
			if (!settings.Read("type", &rule_type_name)) {
				break;
			}

			PortForwardRule rule{};
			if (rule_type_name == "TCP") {
				rule.type = PORT_FORWARD_TCP;
			} else if (rule_type_name == "UDP") {
				rule.type = PORT_FORWARD_UDP;
			} else {
				error("Unknown port forward type, must be TCP or UDP");
				continue;
			}

			long emu_port = 0;
			long host_port = 0;
			settings.Read("emu_port", &emu_port, 0L);
			settings.Read("host_port", &host_port, 0L);

			if (emu_port <= 0 || emu_port > 65535) {
				error("Invalid port forward emu port");
				continue;
			}
			if (host_port <= 0 || host_port > 65535) {
				error("Invalid port forward host port");
				continue;
			}

			rule.emu_port = static_cast<uint16_t>(emu_port);
			rule.host_port = static_cast<uint16_t>(host_port);
			rpcemu_nat_forward_add(rule);
		}
		return;
	}

	for (int i = 0; i < MAX_PORT_FORWARDS; ++i) {
		settings.SetPath(wxString::Format("/nat_port_forward_rules/%d", i));

		wxString rule_type_name;
		if (!settings.Read("type", &rule_type_name)) {
			break;
		}

		PortForwardRule rule{};
		if (rule_type_name == "TCP") {
			rule.type = PORT_FORWARD_TCP;
		} else if (rule_type_name == "UDP") {
			rule.type = PORT_FORWARD_UDP;
		} else {
			error("Unknown port forward type, must be TCP or UDP");
			continue;
		}

		long emu_port = 0;
		long host_port = 0;
		settings.Read("emu_port", &emu_port, 0L);
		settings.Read("host_port", &host_port, 0L);

		if (emu_port <= 0 || emu_port > 65535) {
			error("Invalid port forward emu port");
			continue;
		}
		if (host_port <= 0 || host_port > 65535) {
			error("Invalid port forward host port");
			continue;
		}

		rule.emu_port = static_cast<uint16_t>(emu_port);
		rule.host_port = static_cast<uint16_t>(host_port);
		rpcemu_nat_forward_add(rule);
	}
}

static void config_nat_rules_save(wxFileConfig &settings)
{
	int count = 0;
	for (int i = 0; i < MAX_PORT_FORWARDS; ++i) {
		if (port_forward_rules[i].type != PORT_FORWARD_NONE) {
			++count;
		}
	}

	settings.SetPath("/nat_port_forward_rules");
	settings.Write("size", static_cast<long>(count));

	int itemnum = 0;
	for (int i = 0; i < MAX_PORT_FORWARDS; ++i) {
		if (port_forward_rules[i].type == PORT_FORWARD_NONE) {
			continue;
		}

		settings.SetPath(wxString::Format("/nat_port_forward_rules/%d", itemnum++));
		settings.Write("type", port_forward_rules[i].type == PORT_FORWARD_TCP ? "TCP" : "UDP");
		settings.Write("emu_port", static_cast<long>(port_forward_rules[i].emu_port));
		settings.Write("host_port", static_cast<long>(port_forward_rules[i].host_port));
	}
}

static void podule_config_load(wxFileConfig &settings)
{
	podule_cfg_reset();

	/* Slot assignments: [Podules] slot0=..., slot1=... */
	settings.SetPath("/Podules");
	for (int i = 0; i < PODULE_CONFIG_SLOTS; i++) {
		wxString val;
		if (settings.Read(wxString::Format("slot%d", i), &val) && !val.IsEmpty()) {
			podule_cfg_set_slot(i, val.utf8_str().data());
		}
	}

	/* Per-podule key/value store: [PoduleConfig/<section>] key=value.
	   Collect the group names first - changing SetPath mid-enumeration would
	   invalidate the group iterator. */
	settings.SetPath("/PoduleConfig");
	wxArrayString groups;
	wxString group;
	long gidx;
	for (bool c = settings.GetFirstGroup(group, gidx); c; c = settings.GetNextGroup(group, gidx)) {
		groups.Add(group);
	}
	for (size_t gi = 0; gi < groups.GetCount(); gi++) {
		const wxString &section = groups[gi];
		settings.SetPath("/PoduleConfig/" + section);

		wxString entry;
		long eidx;
		for (bool e = settings.GetFirstEntry(entry, eidx); e; e = settings.GetNextEntry(entry, eidx)) {
			wxString value;
			settings.Read(entry, &value);
			podule_cfg_set_string(section.utf8_str().data(),
			                      entry.utf8_str().data(),
			                      value.utf8_str().data());
		}
		settings.SetPath("/PoduleConfig");
	}
}

static void podule_config_save(wxFileConfig &settings)
{
	settings.SetPath("/Podules");
	for (int i = 0; i < PODULE_CONFIG_SLOTS; i++) {
		const char *name = podule_cfg_get_slot(i);
		settings.Write(wxString::Format("slot%d", i), wxString::FromUTF8(name ? name : ""));
	}

	const int n = podule_cfg_entry_count();
	for (int i = 0; i < n; i++) {
		const char *section;
		const char *key;
		const char *value;
		if (podule_cfg_get_entry(i, &section, &key, &value)) {
			settings.SetPath(wxString("/PoduleConfig/") + wxString::FromUTF8(section));
			settings.Write(wxString::FromUTF8(key), wxString::FromUTF8(value));
		}
	}
}

extern "C" void config_load(Config *cfg)
{
	config_load_from_path(cfg, config_get_path());
}

static void config_replace_strdup(char **field, const wxString &value)
{
	if (*field != nullptr) {
		free(*field);
		*field = nullptr;
	}
	if (!value.empty()) {
		*field = strdup(value.utf8_str().data());
	}
}

static void config_free_heap_strings(Config *cfg)
{
	free(cfg->username);
	free(cfg->ipaddress);
	free(cfg->macaddress);
	free(cfg->bridgename);
	free(cfg->network_capture);
	cfg->username = nullptr;
	cfg->ipaddress = nullptr;
	cfg->macaddress = nullptr;
	cfg->bridgename = nullptr;
	cfg->network_capture = nullptr;
}

extern "C" void config_deep_copy(Config *dest, const Config *src)
{
	config_free_heap_strings(dest);
	memcpy(dest, src, sizeof(Config));
	dest->username = src->username ? strdup(src->username) : nullptr;
	dest->ipaddress = src->ipaddress ? strdup(src->ipaddress) : nullptr;
	dest->macaddress = src->macaddress ? strdup(src->macaddress) : nullptr;
	dest->bridgename = src->bridgename ? strdup(src->bridgename) : nullptr;
	dest->network_capture = src->network_capture ? strdup(src->network_capture) : nullptr;
}

extern "C" void config_sync_machine_edit_to_copy(Config *dest, const Config *src)
{
	if (src->name[0] != '\0') {
		strncpy(dest->name, src->name, sizeof(dest->name) - 1);
		dest->name[sizeof(dest->name) - 1] = '\0';
	}

	strncpy(dest->rom_dir, src->rom_dir, sizeof(dest->rom_dir) - 1);
	dest->rom_dir[sizeof(dest->rom_dir) - 1] = '\0';

	dest->mem_size = src->mem_size;
	dest->vram_size = src->vram_size;
	dest->model = src->model;
	dest->refresh = src->refresh;
	dest->network_type = src->network_type;

	if (src->bridgename != nullptr) {
		config_replace_strdup(&dest->bridgename, wxString::FromUTF8(src->bridgename));
	} else {
		config_replace_strdup(&dest->bridgename, wxEmptyString);
	}
	if (src->ipaddress != nullptr) {
		config_replace_strdup(&dest->ipaddress, wxString::FromUTF8(src->ipaddress));
	} else {
		config_replace_strdup(&dest->ipaddress, wxEmptyString);
	}
}

extern "C" void config_apply_machine_edit(Config *cfg, const char *name, const char *rom_dir,
                                          unsigned mem_size, unsigned vram_size, int refresh,
                                          NetworkType network_type, const char *bridgename,
                                          const char *ipaddress)
{
	if (name != nullptr && name[0] != '\0') {
		strncpy(cfg->name, name, sizeof(cfg->name) - 1);
		cfg->name[sizeof(cfg->name) - 1] = '\0';
	}

	if (rom_dir != nullptr) {
		strncpy(cfg->rom_dir, rom_dir, sizeof(cfg->rom_dir) - 1);
		cfg->rom_dir[sizeof(cfg->rom_dir) - 1] = '\0';
	}

	cfg->mem_size = mem_size;
	cfg->vram_size = vram_size;
	cfg->refresh = refresh;
	cfg->network_type = network_type;

	if (bridgename != nullptr) {
		config_replace_strdup(&cfg->bridgename, wxString::FromUTF8(bridgename));
	} else {
		config_replace_strdup(&cfg->bridgename, wxEmptyString);
	}
	if (ipaddress != nullptr) {
		config_replace_strdup(&cfg->ipaddress, wxString::FromUTF8(ipaddress));
	} else {
		config_replace_strdup(&cfg->ipaddress, wxEmptyString);
	}
}

extern "C" void config_load_from_path(Config *cfg, const char *path)
{
	wxFileConfig settings(wxEmptyString, wxEmptyString,
	                      wxString::FromUTF8(path), wxEmptyString,
	                      wxCONFIG_USE_RELATIVE_PATH);
	ConfigFileUseGeneralGroup(settings);

	wxString key;
	long index = 0;
	while (settings.GetNextEntry(key, index)) {
		wxString value;
		settings.Read(key, &value);
		rpclog("config_load: %s = \"%s\"\n", key.utf8_str().data(), value.utf8_str().data());
	}

	wxString sText;
	settings.Read("name", &sText, wxEmptyString);
	if (snprintf(cfg->name, sizeof(cfg->name), "%s", sText.utf8_str().data()) >= (int) sizeof(cfg->name)) {
		rpclog("config_load: name too long - truncated\n");
	}

	if (cfg->name[0] != '\0') {
		rpcemu_set_machine_datadir(cfg->name);
	} else {
		const wxString baseName = wxFileName(wxString::FromUTF8(path)).GetName();
		strncpy(cfg->name, baseName.utf8_str().data(), sizeof(cfg->name) - 1);
		cfg->name[sizeof(cfg->name) - 1] = '\0';
		rpcemu_set_machine_datadir(cfg->name);
	}

	settings.Read("hd4_path", &sText, wxEmptyString);
	if (snprintf(cfg->hd4_path, sizeof(cfg->hd4_path), "%s", sText.utf8_str().data()) >= (int) sizeof(cfg->hd4_path)) {
		rpclog("config_load: hd4_path too long - truncated\n");
		cfg->hd4_path[0] = '\0';
	}

	settings.Read("rom_dir", &sText, wxEmptyString);
	if (snprintf(cfg->rom_dir, sizeof(cfg->rom_dir), "%s", sText.utf8_str().data()) >= (int) sizeof(cfg->rom_dir)) {
		rpclog("config_load: rom_dir too long - truncated\n");
		cfg->rom_dir[0] = '\0';
	}

	settings.Read("mem_size", &sText, "16");
	const char *p = sText.utf8_str().data();
	if (!strcmp(p, "4")) {
		cfg->mem_size = 4;
	} else if (!strcmp(p, "8")) {
		cfg->mem_size = 8;
	} else if (!strcmp(p, "32")) {
		cfg->mem_size = 32;
	} else if (!strcmp(p, "64")) {
		cfg->mem_size = 64;
	} else if (!strcmp(p, "128")) {
		cfg->mem_size = 128;
	} else if (!strcmp(p, "256")) {
		cfg->mem_size = 256;
	} else if (!strcmp(p, "512")) {
		cfg->mem_size = 512;
	} else {
		cfg->mem_size = 16;
	}

	settings.Read("vram_size", &sText, wxEmptyString);
	if (sText == "0") {
		cfg->vram_size = 0;
	} else if (sText == "2") {
		cfg->vram_size = 2;
	} else if (sText == "4") {
		cfg->vram_size = 4; /* Phoebe */
	} else if (sText == "16") {
		cfg->vram_size = 16;
	} else {
		cfg->vram_size = 8;
	}

	settings.Read("model", &sText, wxEmptyString);
	if (sText == "RPCARM610") {
		sText = "RPC610";
	} else if (sText == "RPCARM710") {
		sText = "RPC710";
	} else if (sText == "RPCARM810") {
		sText = "RPC810";
	}

	Model model = Model_RPCARM710;
	p = sText.utf8_str().data();
	if (p != nullptr) {
		for (int i = 0; i < Model_MAX; ++i) {
			if (strcasecmp(p, models[i].name_config) == 0) {
				model = static_cast<Model>(i);
				break;
			}
		}
	}

	cfg->model = model;
	rpcemu_model_changed(model);

	if (model == Model_A7000 || model == Model_A7000plus) {
		cfg->vram_size = 0;
	}
	if (model == Model_Phoebe) {
		cfg->mem_size = 256;
		cfg->vram_size = 4;
	}
	/* Kinetic + VRAM > 2MB faults on some ROMs (HAL physical-map bug), so it is
	   fixed at 2MB. The graphics card is the answer to the ceiling that leaves,
	   not a larger figure here - see config_apply() in rpcemu.c. */
	if (model == Model_Kinetic) {
		cfg->vram_size = 2;
	}

	machine_cmos_sync(cfg->name, model, cfg->mem_size, cfg->vram_size);

	long value = 0;
	settings.Read("sound_enabled", &value, 1L);
	cfg->soundenabled = static_cast<int>(value);
	settings.Read("refresh_rate", &value, 60L);
	cfg->refresh = static_cast<int>(value);
	settings.Read("cdrom_enabled", &value, 0L);
	cfg->cdromenabled = static_cast<int>(value);
	settings.Read("cdrom_type", &value, 0L);
	cfg->cdromtype = static_cast<int>(value);

	settings.Read("cdrom_iso", &sText, wxEmptyString);
	if (snprintf(cfg->isoname, sizeof(cfg->isoname), "%s", sText.utf8_str().data()) >= (int) sizeof(cfg->isoname)) {
		rpclog("config_load: cdrom_iso path too long - ignored\n");
		cfg->isoname[0] = '\0';
	}

	settings.Read("mouse_following", &value, 1L);
	cfg->mousehackon = static_cast<int>(value);
	/* Follow-mouse is always on: the UI toggle is hidden and the capture path is
	   dormant. Force it on regardless of any older stored value. */
	cfg->mousehackon = 1;
	settings.Read("mouse_twobutton", &value, 0L);
	cfg->mousetwobutton = static_cast<int>(value);

	settings.Read("network_type", &sText, "off");
	if (sText.CmpNoCase("off") == 0) {
		cfg->network_type = NetworkType_Off;
	} else if (sText.CmpNoCase("nat") == 0) {
		cfg->network_type = NetworkType_NAT;
	} else if (sText.CmpNoCase("iptunnelling") == 0) {
		cfg->network_type = NetworkType_IPTunnelling;
	} else if (sText.CmpNoCase("ethernetbridging") == 0) {
		cfg->network_type = NetworkType_EthernetBridging;
	} else {
		rpclog("Unknown network_type '%s', defaulting to off\n", sText.utf8_str().data());
		cfg->network_type = NetworkType_Off;
	}

	settings.Read("username", &sText, wxEmptyString);
	config_replace_strdup(&cfg->username, sText);
	settings.Read("ipaddress", &sText, wxEmptyString);
	config_replace_strdup(&cfg->ipaddress, sText);
	settings.Read("macaddress", &sText, wxEmptyString);
	config_replace_strdup(&cfg->macaddress, sText);
	settings.Read("bridgename", &sText, wxEmptyString);
	config_replace_strdup(&cfg->bridgename, sText);

	settings.Read("cpu_idle", &value, 0L);
	cfg->cpu_idle = static_cast<int>(value);
	settings.Read("show_fullscreen_message", &value, 1L);
	cfg->show_fullscreen_message = static_cast<int>(value);
	settings.Read("integer_scaling", &value, 0L);
	cfg->integer_scaling = static_cast<int>(value);
	settings.Read("fit_to_window", &value, 0L);
	cfg->fit_to_window = static_cast<int>(value);
	settings.Read("follow_host_display", &value, 0L);
	cfg->follow_host_display = static_cast<int>(value);
	settings.Read("gfxcard_enabled", &value, 0L);
	cfg->gfxcard_enabled = static_cast<int>(value);
	settings.Read("gfxcard_boot_display", &value, 0L);
	cfg->gfxcard_boot_display = static_cast<int>(value);
	settings.Read("start_fullscreen", &value, 0L);
	cfg->start_fullscreen = static_cast<int>(value);
	settings.Read("suspend_on_exit", &value, 0L);
	cfg->suspend_on_exit = static_cast<int>(value);
	settings.Read("vnc_enabled", &value, 0L);
	cfg->vnc_enabled = static_cast<int>(value);
	settings.Read("vnc_port", &value, 5900L);
	cfg->vnc_port = static_cast<int>(value);
	settings.Read("vnc_password", &sText, wxEmptyString);
	strncpy(cfg->vnc_password, sText.utf8_str().data(), sizeof(cfg->vnc_password) - 1);
	cfg->vnc_password[sizeof(cfg->vnc_password) - 1] = '\0';

	settings.Read("hostcmd_enabled", &value, 1L);
	cfg->hostcmd_enabled = static_cast<int>(value);
	settings.Read("clipboard_enabled", &value, 0L);
	cfg->clipboard_enabled = static_cast<int>(value);
	settings.Read("hostcmd_socket", &sText, wxEmptyString);
	strncpy(cfg->hostcmd_socket, sText.utf8_str().data(), sizeof(cfg->hostcmd_socket) - 1);
	cfg->hostcmd_socket[sizeof(cfg->hostcmd_socket) - 1] = '\0';

	settings.Read("debug_enabled", &value, 1L);
	cfg->debug_enabled = static_cast<int>(value);
	settings.Read("debug_socket", &sText, wxEmptyString);
	strncpy(cfg->debug_socket, sText.utf8_str().data(), sizeof(cfg->debug_socket) - 1);
	cfg->debug_socket[sizeof(cfg->debug_socket) - 1] = '\0';

	settings.Read("network_capture", &sText, wxEmptyString);
	config_replace_strdup(&cfg->network_capture, sText);

	config_nat_rules_load(settings);
	peripheral_config_load(settings);
	podule_config_load(settings);
}

extern "C" void config_save(Config *cfg)
{
	config_save_to_path(cfg, config_get_path());
}

extern "C" void config_save_to_path(Config *cfg, const char *path)
{
	wxFileConfig settings(wxEmptyString, wxEmptyString,
	                      wxString::FromUTF8(path), wxEmptyString,
	                      wxCONFIG_USE_RELATIVE_PATH);
	settings.DeleteAll();
	ConfigFileUseGeneralGroup(settings);

	settings.Write("name", wxString(cfg->name, wxConvUTF8));
	settings.Write("hd4_path", wxString(cfg->hd4_path, wxConvUTF8));
	settings.Write("rom_dir", wxString(cfg->rom_dir, wxConvUTF8));

	const wxString mem_size_str = wxString::Format("%u", cfg->mem_size);
	settings.Write("mem_size", mem_size_str);
	/* Write the CONFIGURED model (cfg->model), not the running machine.model:
	   editing a running machine's model updates cfg->model but not machine.model
	   (which needs a relaunch), and writing machine.model here would revert the
	   edit on save. Guard the index in case it is uninitialised. */
	{
		Model m = cfg->model;
		if (m < 0 || m >= Model_MAX) {
			m = machine.model;
		}
		settings.Write("model", wxString(models[m].name_config, wxConvUTF8));
	}
	settings.Write("vram_size", wxString::Format("%u", cfg->vram_size));

	settings.Write("sound_enabled", static_cast<long>(cfg->soundenabled));
	settings.Write("refresh_rate", static_cast<long>(cfg->refresh));
	settings.Write("cdrom_enabled", static_cast<long>(cfg->cdromenabled));
	settings.Write("cdrom_type", static_cast<long>(cfg->cdromtype));
	settings.Write("cdrom_iso", wxString(cfg->isoname, wxConvUTF8));
	settings.Write("mouse_following", static_cast<long>(cfg->mousehackon));
	settings.Write("mouse_twobutton", static_cast<long>(cfg->mousetwobutton));

	char s[256];
	switch (cfg->network_type) {
	case NetworkType_Off:              snprintf(s, sizeof(s), "off"); break;
	case NetworkType_NAT:              snprintf(s, sizeof(s), "nat"); break;
	case NetworkType_EthernetBridging: snprintf(s, sizeof(s), "ethernetbridging"); break;
	case NetworkType_IPTunnelling:     snprintf(s, sizeof(s), "iptunnelling"); break;
	default:                           snprintf(s, sizeof(s), "off"); break;
	}
	settings.Write("network_type", wxString(s, wxConvUTF8));

	settings.Write("username", cfg->username ? wxString(cfg->username, wxConvUTF8) : wxString());
	settings.Write("ipaddress", cfg->ipaddress ? wxString(cfg->ipaddress, wxConvUTF8) : wxString());
	settings.Write("macaddress", cfg->macaddress ? wxString(cfg->macaddress, wxConvUTF8) : wxString());
	settings.Write("bridgename", cfg->bridgename ? wxString(cfg->bridgename, wxConvUTF8) : wxString());
	settings.Write("cpu_idle", static_cast<long>(cfg->cpu_idle));
	settings.Write("show_fullscreen_message", static_cast<long>(cfg->show_fullscreen_message));
	settings.Write("integer_scaling", static_cast<long>(cfg->integer_scaling));
	settings.Write("fit_to_window", static_cast<long>(cfg->fit_to_window));
	settings.Write("follow_host_display", static_cast<long>(cfg->follow_host_display));
	settings.Write("gfxcard_enabled", static_cast<long>(cfg->gfxcard_enabled));
	settings.Write("gfxcard_boot_display", static_cast<long>(cfg->gfxcard_boot_display));
	settings.Write("start_fullscreen", static_cast<long>(cfg->start_fullscreen));
	settings.Write("suspend_on_exit", static_cast<long>(cfg->suspend_on_exit));
	settings.Write("vnc_enabled", static_cast<long>(cfg->vnc_enabled));
	settings.Write("vnc_port", static_cast<long>(cfg->vnc_port));
	settings.Write("vnc_password", wxString(cfg->vnc_password, wxConvUTF8));
	settings.Write("hostcmd_enabled", static_cast<long>(cfg->hostcmd_enabled));
	settings.Write("clipboard_enabled", static_cast<long>(cfg->clipboard_enabled));
	settings.Write("hostcmd_socket", wxString(cfg->hostcmd_socket, wxConvUTF8));
	settings.Write("debug_enabled", static_cast<long>(cfg->debug_enabled));
	settings.Write("debug_socket", wxString(cfg->debug_socket, wxConvUTF8));
	settings.Write("network_capture", cfg->network_capture ? wxString(cfg->network_capture, wxConvUTF8) : wxString());

	config_nat_rules_save(settings);
	peripheral_config_save(settings);
	podule_config_save(settings);
	settings.Flush();
}
