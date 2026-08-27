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
#include "app_settings.h"
#include "peripheral_config.h"
#include "podule_config.h"
}

#include <wx/arrstr.h>

#include "display_options.h"
#include "mac_address_input.h"

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
	char machine_dir[512];
	char meta_path[560];	/* the directory plus the longest name below */
	char cmos_path[512];

	rpcemu_machine_datadir_for(machine_dir, sizeof(machine_dir), machine_name);
	snprintf(meta_path, sizeof(meta_path), "%semulator.meta", machine_dir);

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

/*
 * A USB port's setting, as it is written in a machine's configuration.
 *
 * It is a word rather than a number because a port can hold a real device from
 * the host, and that needs saying which one: "host:046d:c077". A device is
 * remembered by what it says it is rather than by where it is plugged in, so
 * moving it to another socket does not lose it.
 *
 * The numbers earlier builds wrote are still understood, so a machine set up
 * before this existed comes back with the same thing plugged in.
 */
static void ParseUsbPort(const wxString &text, int *kind, char *host_id,
                         size_t host_id_size)
{
	const wxString value = text.Strip(wxString::both).Lower();

	*kind = UsbAttachment_None;
	host_id[0] = '\0';

	if (value.empty() || value == "0" || value == "none") {
		return;
	}

	/* The synthesised gamepad that used to be offered here is gone, along with
	   the reason for it. A machine that still names it comes up with that port
	   empty rather than refusing to load. */
	if (value == "1" || value == "gamepad") {
		return;
	}

	if (value.StartsWith("host:")) {
		const wxString id = value.Mid(5);
		unsigned vendor, product;

		/* Only accept an identifier that can actually name a device, so a
		   damaged line leaves the port empty rather than half-configured. */
		if (sscanf(id.utf8_str().data(), "%4x:%4x", &vendor, &product) == 2) {
			*kind = UsbAttachment_Host;
			snprintf(host_id, host_id_size, "%04x:%04x", vendor & 0xffff,
			    product & 0xffff);
		}
	}
}

static wxString FormatUsbPort(int kind, const char *host_id)
{
	switch (kind) {
	case UsbAttachment_Host:
		if (host_id != nullptr && host_id[0] != '\0') {
			return wxString::Format("host:%s", host_id);
		}
		return "none";

	default:
		return "none";
	}
}

static void config_free_heap_strings(Config *cfg)
{
	free(cfg->macaddress);
	free(cfg->network_capture);
	cfg->macaddress = nullptr;
	cfg->network_capture = nullptr;
}

extern "C" void config_deep_copy(Config *dest, const Config *src)
{
	config_free_heap_strings(dest);
	memcpy(dest, src, sizeof(Config));
	dest->macaddress = src->macaddress ? strdup(src->macaddress) : nullptr;
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
	strncpy(dest->openbus_card, src->openbus_card, sizeof(dest->openbus_card) - 1);
	dest->openbus_card[sizeof(dest->openbus_card) - 1] = '\0';
	dest->openbus_ram_kb = src->openbus_ram_kb;

	/* JSON networking travels with the rest of the machine's networking:
	   the editor writes it, and this is what carries it to the running copy. */
	dest->json_net_enabled = src->json_net_enabled;
	dest->json_net_port = src->json_net_port;
	dest->community_net_enabled = src->community_net_enabled;
	strncpy(dest->json_net_host, src->json_net_host, sizeof(dest->json_net_host) - 1);
	dest->json_net_host[sizeof(dest->json_net_host) - 1] = '\0';

	/* The display choices, so the Settings menu and the window agree with what
	   the editor was just told. Without these the editor wrote them to the
	   configuration and to the live config, and the frame's copy - which is what
	   the menu ticks and the drawing rule are read from - kept the old values:
	   the two views of one setting disagreed the moment either was used. */
	dest->display_scaling = src->display_scaling;
	dest->screen_size_x = src->screen_size_x;
	dest->screen_size_y = src->screen_size_y;
}

extern "C" void config_apply_machine_edit(Config *cfg, const char *name, const char *rom_dir,
                                          unsigned mem_size, unsigned vram_size, int refresh,
                                          NetworkType network_type)
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
}

/* Set once a machine's configuration has been read, so whatever starts the VNC
   server or the command socket can tell "this machine says nothing about it" from
   "there is no machine yet". See config_machine_loaded() in rpcemu.h. */
static bool machine_config_loaded = false;

extern "C" int config_machine_loaded(void)
{
	return machine_config_loaded ? 1 : 0;
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
	bool macaddress_generated = false;
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

	settings.Read("hostfs_path", &sText, wxEmptyString);
	if (snprintf(cfg->hostfs_path, sizeof(cfg->hostfs_path), "%s", sText.utf8_str().data()) >= (int) sizeof(cfg->hostfs_path)) {
		/* Emptied rather than truncated: a truncated path is a different
		   directory, and HostFS would create it and write the guest's files
		   there. Falling back to the default is the safe answer. */
		rpclog("config_load: hostfs_path too long - using the default\n");
		cfg->hostfs_path[0] = '\0';
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

	/*
	 * On by default, and off means captured-pointer mode.
	 *
	 * This used to read the stored value and then overwrite it with 1, from when
	 * the menu toggle was hidden. That went further than hiding a menu item: it
	 * meant a machine could not be put into capture mode by any route at all,
	 * including editing this file by hand. Games that take the pointer over
	 * themselves need it, so the stored value is honoured again.
	 */
	settings.Read("mouse_following", &value, 1L);
	cfg->mousehackon = static_cast<int>(value);
	settings.Read("mouse_twobutton", &value, 0L);
	cfg->mousetwobutton = static_cast<int>(value);

	settings.Read("network_type", &sText, "off");
	if (sText.CmpNoCase("off") == 0) {
		cfg->network_type = NetworkType_Off;
	} else if (sText.CmpNoCase("nat") == 0) {
		cfg->network_type = NetworkType_NAT;
	} else if (sText.CmpNoCase("iptunnelling") == 0 ||
	           sText.CmpNoCase("ethernetbridging") == 0) {
		/*
		 * Bridging and tunnelling are gone. A machine set to either wanted
		 * networking, so it gets NAT rather than nothing: NAT needs no host
		 * configuration and, with port forwarding, covers what they were
		 * chosen for. Said in the log because the machine's address changes.
		 */
		rpclog("Networking: '%s' is no longer supported, using NAT instead\n",
		       sText.utf8_str().data());
		cfg->network_type = NetworkType_NAT;
	} else {
		rpclog("Unknown network_type '%s', defaulting to off\n", sText.utf8_str().data());
		cfg->network_type = NetworkType_Off;
	}

	/* A machine that has never had a MAC address is given one now, and it is
	   written straight back so it stays that machine's address. */
	settings.Read("macaddress", &sText, wxEmptyString);
	if (sText.empty()) {
		sText = wxString::FromUTF8(MacAddressInput::Generate().c_str());
		macaddress_generated = true;
		rpclog("Networking: this machine had no MAC address, so it has been "
		       "given %s\n", sText.utf8_str().data());
	}
	config_replace_strdup(&cfg->macaddress, sText);

	settings.Read("cpu_idle", &value, 0L);
	cfg->cpu_idle = static_cast<int>(value);
	settings.Read("show_fullscreen_message", &value, 1L);
	cfg->show_fullscreen_message = static_cast<int>(value);
	/*
	 * The display settings, and the switches they grew out of.
	 *
	 * A configuration written before any of this has integer_scaling,
	 * fit_to_window and follow_host_display; one written between then and now may
	 * also have a "screen_size" naming a policy - best for this display, or follow
	 * the window. Those policies are gone: RISC OS accepts only the screen modes
	 * its monitor definition declares, so a desktop that chased the window landed
	 * on a coarse and unpredictable set of sizes and moved the window about while
	 * it did it. What is left is a plain resolution.
	 *
	 * Anything that named a policy therefore converts to no resolution at all,
	 * which MainFrame::StartEmulator() then fills in with the best one for this
	 * display - the same answer the policy would have given on its first run, but
	 * recorded once instead of re-decided behind the user's back.
	 */
	{
		long old_integer = 0, old_fit = 0;

		settings.Read("integer_scaling", &old_integer, 0L);
		settings.Read("fit_to_window", &old_fit, 0L);

		/* fit_to_window became "fill the window", which is gone too: it stretched
		   the desktop to cover a window that did not match it, and with the window
		   now being the desktop's size there is nothing to cover. Actual size is
		   what it becomes. */
		const long scaling_default = (old_integer != 0 && old_fit == 0)
		    ? DisplayScaling_WholeMultiples : DisplayScaling_ActualSize;

		settings.Read("display_scaling", &value, scaling_default);
		cfg->display_scaling = DisplayOptions::ClampDisplayScaling((int) value);

		settings.Read("screen_size_x", &value, 0L);
		cfg->screen_size_x = (unsigned) (value < 0 ? 0 : value);
		settings.Read("screen_size_y", &value, 0L);
		cfg->screen_size_y = (unsigned) (value < 0 ? 0 : value);

		/* A size that is not a whole mode is no size at all. */
		if (cfg->screen_size_x == 0 || cfg->screen_size_y == 0) {
			cfg->screen_size_x = 0;
			cfg->screen_size_y = 0;
		}
	}
	settings.Read("gfxcard_enabled", &value, 0L);
	cfg->gfxcard_enabled = static_cast<int>(value);
	settings.Read("gfxcard_boot_display", &value, 0L);
	cfg->gfxcard_boot_display = static_cast<int>(value);
	/* Defaults on, including for a machine whose configuration predates it:
	   what it does is invisible except in how long it takes. */
	settings.Read("accelerators_enabled", &value, 1L);
	cfg->accelerators_enabled = static_cast<int>(value);
	/* USB ports. Nothing is plugged in unless the machine says so, since a
	   device appearing on its own would be a surprise to the guest. */
	for (int port = 0; port < USB_PORTS; port++) {
		settings.Read(wxString::Format("usb_port%d", port + 1), &sText,
		    wxEmptyString);
		ParseUsbPort(sText, &cfg->usb_port[port], cfg->usb_host[port],
		    sizeof(cfg->usb_host[port]));
	}
	/* The OPEN Bus co-processor card. Absent by default, and an unknown name
	   is left as it was written rather than corrected: openbus_coproc_fit()
	   is what decides whether a name means anything, and it says so in the
	   log if it does not. */
	settings.Read("openbus_card", &sText, wxEmptyString);
	strncpy(cfg->openbus_card, sText.utf8_str().data(),
	    sizeof(cfg->openbus_card) - 1);
	cfg->openbus_card[sizeof(cfg->openbus_card) - 1] = '\0';

	/* Zero, which is what an older machine's configuration reads as, means the
	   core's own default. The card clamps anything larger than the fitted
	   processor can address. */
	{
		long ram_kb = 0;

		settings.Read("openbus_ram_kb", &ram_kb, 0);
		cfg->openbus_ram_kb = (ram_kb > 0) ? (unsigned) ram_kb : 0u;
	}
	settings.Read("start_fullscreen", &value, 0L);
	cfg->start_fullscreen = static_cast<int>(value);
	settings.Read("suspend_on_exit", &value, 0L);
	cfg->suspend_on_exit = static_cast<int>(value);
	/*
	 * The ways in to a machine - its VNC server and its HostCmd socket - belong to
	 * that machine, so this is where they are settled and this is what wins.
	 *
	 * Three layers, weakest first: the built-in defaults set here, then the app
	 * settings file if it has anything to say, then this machine's own file. The
	 * middle layer is what a headless run uses before any machine is chosen (the
	 * selector has to be reachable before there is a machine to ask), and it
	 * doubles as the default for a machine whose file predates these keys being
	 * written again. The command line is applied after all three, at the end of
	 * this function.
	 */
	cfg->vnc_enabled = 0;
	cfg->vnc_port = 5900;
	cfg->vnc_password[0] = '\0';
	cfg->vnc_password_readonly[0] = '\0';
	cfg->hostcmd_enabled = 1;
	cfg->hostcmd_socket[0] = '\0';

	{
		const int applied = app_settings_load(rpcemu_get_datadir(), cfg);

		if (applied < 0) {
			/* Reported rather than passed over: otherwise the user's settings are
			   being ignored in silence. */
			rpclog("config_load: cannot read %s, using this machine's values\n",
			    app_settings_path(rpcemu_get_datadir()));
		} else if (applied > 0) {
			rpclog("config_load: %d default%s from %s\n", applied,
			    applied == 1 ? "" : "s",
			    app_settings_path(rpcemu_get_datadir()));
		}
	}

	/* Only where the machine says so, hence HasEntry rather than a default: a key
	   the machine's file does not mention must leave the layer below it standing,
	   and Read() with a default cannot tell the two apart. */
	if (settings.HasEntry("vnc_enabled")) {
		settings.Read("vnc_enabled", &value, 0L);
		cfg->vnc_enabled = static_cast<int>(value);
	}
	if (settings.HasEntry("vnc_port")) {
		settings.Read("vnc_port", &value, 5900L);
		cfg->vnc_port = static_cast<int>(value);
	}
	if (settings.HasEntry("vnc_password")) {
		settings.Read("vnc_password", &sText, wxEmptyString);
		strncpy(cfg->vnc_password, sText.utf8_str().data(), sizeof(cfg->vnc_password) - 1);
		cfg->vnc_password[sizeof(cfg->vnc_password) - 1] = '\0';
	}
	if (settings.HasEntry("vnc_password_readonly")) {
		settings.Read("vnc_password_readonly", &sText, wxEmptyString);
		strncpy(cfg->vnc_password_readonly, sText.utf8_str().data(),
		    sizeof(cfg->vnc_password_readonly) - 1);
		cfg->vnc_password_readonly[sizeof(cfg->vnc_password_readonly) - 1] = '\0';
	}
	if (settings.HasEntry("hostcmd_enabled")) {
		settings.Read("hostcmd_enabled", &value, 1L);
		cfg->hostcmd_enabled = static_cast<int>(value);
	}
	if (settings.HasEntry("hostcmd_socket")) {
		settings.Read("hostcmd_socket", &sText, wxEmptyString);
		strncpy(cfg->hostcmd_socket, sText.utf8_str().data(), sizeof(cfg->hostcmd_socket) - 1);
		cfg->hostcmd_socket[sizeof(cfg->hostcmd_socket) - 1] = '\0';
	}

	settings.Read("clipboard_enabled", &value, 0L);
	cfg->clipboard_enabled = static_cast<int>(value);

	settings.Read("netcap_enabled", &value, 1L);
	cfg->netcap_enabled = static_cast<int>(value);
	settings.Read("netcap_socket", &sText, wxEmptyString);
	strncpy(cfg->netcap_socket, sText.utf8_str().data(), sizeof(cfg->netcap_socket) - 1);
	cfg->netcap_socket[sizeof(cfg->netcap_socket) - 1] = '\0';

	settings.Read("debug_enabled", &value, 1L);
	cfg->debug_enabled = static_cast<int>(value);
	settings.Read("debug_socket", &sText, wxEmptyString);
	strncpy(cfg->debug_socket, sText.utf8_str().data(), sizeof(cfg->debug_socket) - 1);
	cfg->debug_socket[sizeof(cfg->debug_socket) - 1] = '\0';

	/* JSON networking: the tun/tap server this machine joins, if
	   any. See net_json.h. */
	settings.Read("json_net_enabled", &value, 0L);
	cfg->json_net_enabled = static_cast<int>(value);
	settings.Read("json_net_host", &sText, wxEmptyString);
	strncpy(cfg->json_net_host, sText.utf8_str().data(), sizeof(cfg->json_net_host) - 1);
	cfg->json_net_host[sizeof(cfg->json_net_host) - 1] = '\0';
	settings.Read("json_net_port", &value, 33445L);
	cfg->json_net_port = static_cast<int>(value);
	settings.Read("community_net_enabled", &value, 0L);
	cfg->community_net_enabled = static_cast<int>(value);

	settings.Read("network_capture", &sText, wxEmptyString);
	config_replace_strdup(&cfg->network_capture, sText);

	config_nat_rules_load(settings);
	peripheral_config_load(settings);
	podule_config_load(settings);

	/* Last word, after the machine's own values: several instances sharing a data
	   directory each need their own port and sockets, which only the command line
	   can say. Nothing migrates a machine's keys into the app settings file any
	   more - config_save() writes them back where they came from. */
	app_settings_apply_overrides(cfg);

	/* The one key, not config_save(): the overrides above are in cfg now, and
	   saving the whole structure would record them as the user's own. The group
	   is set again first - podule_config_load() above leaves the path in its
	   own group, and the key would be written there instead. */
	if (macaddress_generated && cfg->macaddress != NULL) {
		ConfigFileUseGeneralGroup(settings);
		settings.Write("macaddress", wxString(cfg->macaddress, wxConvUTF8));
		if (!settings.Flush()) {
			rpclog("Networking: could not record the new MAC address; this "
			       "machine will be given a different one next time\n");
		}
	}

	machine_config_loaded = true;
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

	/*
	 * ★ Captured BEFORE DeleteAll(), which wipes the file and writes it again.
	 *
	 * A setting a command-line option is overriding must come out of the file
	 * unchanged, and simply not writing it does not achieve that - it deletes
	 * the key, because of that DeleteAll(). Which is how the first attempt at
	 * this silently discarded a deliberately configured socket instead of
	 * preserving it. So the file's own value is read first and written back.
	 *
	 * Absence is preserved as absence: a key the file never had stays missing,
	 * which config_load() reads as "use the default".
	 */
	struct PreservedEntry {
		const char *key;
		AppSettingId setting;
		bool present;
		wxString value;
	} preserved[] = {
		{ "vnc_enabled",    APP_SETTING_VNC_ENABLED,    false, wxEmptyString },
		{ "vnc_port",       APP_SETTING_VNC_PORT,       false, wxEmptyString },
		{ "hostcmd_socket", APP_SETTING_HOSTCMD_SOCKET, false, wxEmptyString },
		{ "debug_socket",   APP_SETTING_DEBUG_SOCKET,   false, wxEmptyString },
	};

	ConfigFileUseGeneralGroup(settings);
	for (PreservedEntry &e : preserved) {
		if (app_settings_is_overridden(e.setting) && settings.HasEntry(e.key)) {
			e.present = settings.Read(e.key, &e.value);
		}
	}

	settings.DeleteAll();
	ConfigFileUseGeneralGroup(settings);

	settings.Write("name", wxString(cfg->name, wxConvUTF8));
	settings.Write("hd4_path", wxString(cfg->hd4_path, wxConvUTF8));
	/* Empty when it is the default, which is what keeps a machine's
	   configuration valid after its data folder moves. */
	settings.Write("hostfs_path", wxString(cfg->hostfs_path, wxConvUTF8));
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
	default:                           snprintf(s, sizeof(s), "off"); break;
	}
	settings.Write("network_type", wxString(s, wxConvUTF8));
	settings.Write("json_net_enabled", static_cast<long>(cfg->json_net_enabled));
	settings.Write("json_net_host", wxString(cfg->json_net_host, wxConvUTF8));
	settings.Write("json_net_port", static_cast<long>(cfg->json_net_port));
	settings.Write("community_net_enabled",
	    static_cast<long>(cfg->community_net_enabled));

	settings.Write("macaddress", cfg->macaddress ? wxString(cfg->macaddress, wxConvUTF8) : wxString());
	settings.Write("cpu_idle", static_cast<long>(cfg->cpu_idle));
	settings.Write("show_fullscreen_message", static_cast<long>(cfg->show_fullscreen_message));
	settings.Write("display_scaling", static_cast<long>(cfg->display_scaling));
	settings.Write("screen_size_x", static_cast<long>(cfg->screen_size_x));
	settings.Write("screen_size_y", static_cast<long>(cfg->screen_size_y));
	settings.Write("gfxcard_enabled", static_cast<long>(cfg->gfxcard_enabled));
	settings.Write("gfxcard_boot_display", static_cast<long>(cfg->gfxcard_boot_display));
	for (int port = 0; port < USB_PORTS; port++) {
		settings.Write(wxString::Format("usb_port%d", port + 1),
		    FormatUsbPort(cfg->usb_port[port], cfg->usb_host[port]));
	}
	settings.Write("openbus_card", wxString(cfg->openbus_card, wxConvUTF8));
	settings.Write("openbus_ram_kb", (long) cfg->openbus_ram_kb);
	settings.Write("start_fullscreen", static_cast<long>(cfg->start_fullscreen));
	settings.Write("suspend_on_exit", static_cast<long>(cfg->suspend_on_exit));
	/* The ways in to this machine, written with it: two machines can each have
	   their own VNC port and passwords and their own HostCmd socket, and keep them
	   without being told again on the command line every time. The app settings
	   file supplies these before a machine is chosen, and as the default for a
	   machine whose file has not got them yet. */
	/*
	 * ★ A SETTING A COMMAND-LINE OPTION IS OVERRIDING IS NOT WRITTEN, and the
	 * file's own value is left exactly as it was.
	 *
	 * An override is applied to the live Config so everything downstream sees
	 * it, and this function then writes the Config out - so a per-run option was
	 * being recorded as though the user had chosen it. One run with
	 * --debug-socket left an absolute path in the machine's configuration that
	 * broke the moment the data folder moved, and --vnc-port did the same to the
	 * port, which matters more than it sounds: running two instances with
	 * different ports permanently rewrote both machines' configured port.
	 *
	 * Not writing is deliberately better than writing the pre-override value
	 * back: there is nothing to remember, and wxFileConfig leaves an entry it is
	 * not asked to change untouched.
	 */
	{
		/* Overridden: put back exactly what the file said, or leave the key out
		   if it never had one. Otherwise write the running value as usual. */
		auto write_or_restore = [&](const char *key, const wxString &running) {
			for (const PreservedEntry &e : preserved) {
				if (strcmp(e.key, key) != 0) {
					continue;
				}
				if (app_settings_is_overridden(e.setting)) {
					if (e.present) {
						settings.Write(key, e.value);
					}
					return;
				}
			}
			settings.Write(key, running);
		};

		write_or_restore("vnc_enabled",
		    wxString::Format("%ld", static_cast<long>(cfg->vnc_enabled)));
		write_or_restore("vnc_port",
		    wxString::Format("%ld", static_cast<long>(cfg->vnc_port)));
		settings.Write("vnc_password", wxString(cfg->vnc_password, wxConvUTF8));
		settings.Write("vnc_password_readonly",
		    wxString(cfg->vnc_password_readonly, wxConvUTF8));
		settings.Write("hostcmd_enabled", static_cast<long>(cfg->hostcmd_enabled));
		write_or_restore("hostcmd_socket",
		    wxString(cfg->hostcmd_socket, wxConvUTF8));
		settings.Write("clipboard_enabled",
		    static_cast<long>(cfg->clipboard_enabled));
		settings.Write("netcap_enabled", static_cast<long>(cfg->netcap_enabled));
		write_or_restore("netcap_socket",
		    wxString(cfg->netcap_socket, wxConvUTF8));
		settings.Write("debug_enabled", static_cast<long>(cfg->debug_enabled));
		write_or_restore("debug_socket", wxString(cfg->debug_socket, wxConvUTF8));
	}
	settings.Write("network_capture", cfg->network_capture ? wxString(cfg->network_capture, wxConvUTF8) : wxString());

	config_nat_rules_save(settings);
	peripheral_config_save(settings);
	podule_config_save(settings);
	settings.Flush();
}
