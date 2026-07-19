#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rpcemu.h"
#include "disc.h"
#include "disc_hfe.h"
#include "disc_mfm_common.h"
#include "savestate.h"

static disc_funcs hfe_disc_funcs;

static void hfe_writeback(int drive);

#define TRACK_ENCODING_ISOIBM_MFM 0x00
#define TRACK_ENCODING_AMIGA_MFM  0x01
#define TRACK_ENCODING_ISOIBM_FM  0x02
#define TRACK_ENCODING_EMU_FM     0x03

typedef struct hfe_header_t {
	char signature[8]; /*Should be HXCPICFE*/
	uint8_t revision;
	uint8_t nr_of_tracks;
	uint8_t nr_of_sides;
	uint8_t track_encoding;
	uint16_t bitrate;
	uint16_t floppy_rpm;
	uint8_t floppy_interface_mode;
	uint8_t dnu;
	uint16_t track_list_offset;
	uint8_t write_allowed;
	uint8_t single_step;
	uint8_t track0s0_altencoding;
	uint8_t track0s0_encoding;
	uint8_t track0s1_altencoding;
	uint8_t track0s1_encoding;
} hfe_header_t;

typedef struct hfe_track_t {
	uint16_t offset;
	uint16_t track_len;
} hfe_track_t;

typedef struct hfe_t {
	hfe_header_t header;
	hfe_track_t *tracks;
	mfm_t mfm;
	FILE *f;
	int current_track;
	int write_prot;
} hfe_t;

static hfe_t hfe[4];

static int hfe_drive;

static int hfe_load_header(hfe_t *hfe)
{
	hfe_header_t *header = &hfe->header;

	fread(header->signature, 8, 1, hfe->f);
	header->revision = getc(hfe->f);
	header->nr_of_tracks = getc(hfe->f);
	header->nr_of_sides = getc(hfe->f);
	header->track_encoding = getc(hfe->f);
	fread(&header->bitrate, 2, 1, hfe->f);
	fread(&header->floppy_rpm, 2, 1, hfe->f);
	header->floppy_interface_mode = getc(hfe->f);
	header->dnu = getc(hfe->f);
	fread(&header->track_list_offset, 2, 1, hfe->f);
	header->write_allowed = getc(hfe->f);
	header->single_step = getc(hfe->f);
	header->track0s0_altencoding = getc(hfe->f);
	header->track0s0_encoding = getc(hfe->f);
	header->track0s1_altencoding = getc(hfe->f);
	header->track0s1_encoding = getc(hfe->f);

	if (strncmp(header->signature, "HXCPICFE", 8)) {
		rpclog("HFE signature does not match\n");
		return -1;
	}
	if (header->revision != 0) {
		rpclog("HFE revision %i unsupported\n", header->revision);
		return -1;
	}

	/* nr_of_tracks and nr_of_sides come straight from the (untrusted) image
	   file. A zero in either would make the track-list allocation empty while
	   hfe_seek() still indexes tracks[0..nr_of_tracks-1], reading out of
	   bounds. Reject implausible geometry up front. */
	if (header->nr_of_tracks == 0 || header->nr_of_sides == 0 ||
	    header->nr_of_sides > 2) {
		rpclog("HFE: invalid geometry (%u tracks, %u sides)\n",
		       header->nr_of_tracks, header->nr_of_sides);
		return -1;
	}

//        rpclog("HFE: %i tracks, %i sides\n", header->nr_of_tracks, header->nr_of_sides);
//        rpclog("  track_list_offset: %i\n", header->track_list_offset);
	{
		size_t track_list_size = (size_t) header->nr_of_tracks *
		                         header->nr_of_sides * sizeof(hfe_track_t);

		hfe->tracks = malloc(track_list_size);
		if (hfe->tracks == NULL) {
			rpclog("HFE: could not allocate %zu-byte track list\n",
			       track_list_size);
			return -1;
		}
		memset(hfe->tracks, 0, track_list_size);
		fseek(hfe->f, header->track_list_offset * 0x200, SEEK_SET);
		if (fread(hfe->tracks, track_list_size, 1, hfe->f) != 1) {
			rpclog("HFE: could not read track list\n");
			free(hfe->tracks);
			hfe->tracks = NULL;
			return -1;
		}
	}

	return 0;
}

void hfe_init(void)
{
//        printf("hfe reset\n");
	memset(hfe, 0, sizeof(hfe));
}

void hfe_load(int drive, const char *fn)
{
	hfe[drive].write_prot = 0;
	memset(&hfe[drive], 0, sizeof(hfe_t));
	hfe[drive].f = fopen(fn, "rb+");
	if (!hfe[drive].f) {
		hfe[drive].f = fopen(fn, "rb");
		if (!hfe[drive].f)
			return;
		hfe[drive].write_prot = 1;
	}
	if (hfe_load_header(&hfe[drive])) {
		/* Malformed or unsupported image: don't wire up the drive, or a
		   later seek would dereference the (NULL) track list. */
		fclose(hfe[drive].f);
		hfe[drive].f = NULL;
		return;
	}
	hfe[drive].mfm.write_protected = hfe[drive].write_prot;
	hfe[drive].mfm.writeback = hfe_writeback;

	drive_funcs[drive] = &hfe_disc_funcs;
	//rpclog("Loaded as hfe\n");

	drive_funcs[drive]->seek(drive, disc_get_current_track(drive));
}

static void hfe_close(int drive)
{
	if (hfe[drive].tracks) {
		free(hfe[drive].tracks);
		hfe[drive].tracks = NULL;
	}
	if (hfe[drive].f) {
		fclose(hfe[drive].f);
		hfe[drive].f = NULL;
	}
}

static void do_bitswap(uint8_t *data, int size)
{
	int c;

	for (c = 0; c < size; c++) {
		uint8_t new_val = 0;

		if (data[c] & 0x01)
			new_val |= 0x80;
		if (data[c] & 0x02)
			new_val |= 0x40;
		if (data[c] & 0x04)
			new_val |= 0x20;
		if (data[c] & 0x08)
			new_val |= 0x10;
		if (data[c] & 0x10)
			new_val |= 0x08;
		if (data[c] & 0x20)
			new_val |= 0x04;
		if (data[c] & 0x40)
			new_val |= 0x02;
		if (data[c] & 0x80)
			new_val |= 0x01;

		data[c] = new_val;
	}
}

static void upsample_track(uint8_t *data, int size)
{
	int c;

	for (c = size-1; c >= 0; c--) {
		uint8_t new_data = 0;

		if (data[c] & 0x08)
			new_data |= 0x80;
		if (data[c] & 0x04)
			new_data |= 0x20;
		if (data[c] & 0x02)
			new_data |= 0x08;
		if (data[c] & 0x01)
			new_data |= 0x02;
		data[c*2+1] = new_data;

		new_data = 0;
		if (data[c] & 0x80)
			new_data |= 0x80;
		if (data[c] & 0x40)
			new_data |= 0x20;
		if (data[c] & 0x20)
			new_data |= 0x08;
		if (data[c] & 0x10)
			new_data |= 0x02;
		data[c*2] = new_data;
	}
}

static void downsample_track(uint8_t *data, int size)
{
	int c;

	for (c = 0; c < size; c++) {
		uint8_t new_data = 0;

		if (data[c*2+1] & 0x80)
			new_data |= 0x08;
		if (data[c*2+1] & 0x20)
			new_data |= 0x04;
		if (data[c*2+1] & 0x08)
			new_data |= 0x02;
		if (data[c*2+1] & 0x02)
			new_data |= 0x01;
		if (data[c*2] & 0x80)
			new_data |= 0x80;
		if (data[c*2] & 0x20)
			new_data |= 0x40;
		if (data[c*2] & 0x08)
			new_data |= 0x20;
		if (data[c*2] & 0x02)
			new_data |= 0x10;
		data[c] = new_data;
	}
}

static void hfe_seek(int drive, int track)
{
	hfe_header_t *header = &hfe[drive].header;
	mfm_t *mfm = &hfe[drive].mfm;
	int c;

	if (!hfe[drive].f || hfe[drive].tracks == NULL) {
		memset(mfm->track_data[0], 0, 65536);
		memset(mfm->track_data[1], 0, 65536);
		return;
	}
//        printf("Track start %i\n",track);
	if (track < 0)
		track = 0;
	if (track >= header->nr_of_tracks)
		track = header->nr_of_tracks - 1;

	hfe->current_track = track;

//        rpclog("hfe_seek: drive=%i track=%i\n", drive, track);
//        rpclog("  offset=%04x size=%04x\n", hfe[drive].tracks[track].offset, hfe[drive].tracks[track].track_len);
	fseek(hfe[drive].f, hfe[drive].tracks[track].offset * 0x200, SEEK_SET);
//        rpclog("  start=%06x\n", ftell(hfe[drive].f));
	for (c = 0; c < (hfe[drive].tracks[track].track_len/2); c += 0x100) {
		fread(&mfm->track_data[0][c], 256, 1, hfe[drive].f);
		fread(&mfm->track_data[1][c], 256, 1, hfe[drive].f);
	}
//        rpclog("  end=%06x\n", ftell(hfe[drive].f));
	mfm->track_index[0] = 0;
	mfm->track_index[1] = 0;
	mfm->track_len[0] = (hfe[drive].tracks[track].track_len*8)/2;
	mfm->track_len[1] = (hfe[drive].tracks[track].track_len*8)/2;
	do_bitswap(mfm->track_data[0], (mfm->track_len[0] + 7) / 8);
	do_bitswap(mfm->track_data[1], (mfm->track_len[1] + 7) / 8);

	if (header->bitrate < 400) {
		upsample_track(mfm->track_data[0], (mfm->track_len[0] + 7) / 8);
		upsample_track(mfm->track_data[1], (mfm->track_len[1] + 7) / 8);
		mfm->track_len[0] *= 2;
		mfm->track_len[1] *= 2;
	}

//        rpclog(" SD side 0 Track %i Len %i Index %i\n", track, mfm->track_len[0][0], mfm->track_index[0][0]);
//        rpclog(" SD side 1 Track %i Len %i Index %i\n", track, mfm->track_len[1][0], mfm->track_index[1][0]);
//        rpclog(" DD side 0 Track %i Len %i Index %i\n", track, mfm->track_len[0], mfm->track_index[0]);
//        rpclog(" DD side 1 Track %i Len %i Index %i\n", track, mfm->track_len[1], mfm->track_index[1]);
}

static void hfe_writeback(int drive)
{
	hfe_header_t *header = &hfe[drive].header;
	mfm_t *mfm = &hfe[drive].mfm;
	int track = hfe[drive].current_track;
	uint8_t track_data[2][65536];
	int c;

//        rpclog("hfe_writeback: drive=%i track=%i\n", drive, track);

	for (c = 0; c < 2; c++) {
		int track_len = mfm->track_len[c];
		memcpy(track_data[c], mfm->track_data[c], (track_len + 7) / 8);

		if (header->bitrate < 400) {
			downsample_track(track_data[c], (track_len + 7) / 8);
			track_len /= 2;
		}
		do_bitswap(track_data[c], (track_len + 7) / 8);
	}

	fseek(hfe[drive].f, hfe[drive].tracks[track].offset * 0x200, SEEK_SET);
//        rpclog(" at %06x\n", ftell(hfe[drive].f));
	for (c = 0; c < (hfe[drive].tracks[track].track_len/2); c += 0x100) {
		fwrite(&track_data[0][c], 256, 1, hfe[drive].f);
		fwrite(&track_data[1][c], 256, 1, hfe[drive].f);
	}
}

static void hfe_readsector(int drive, int sector, int track, int side, int density)
{
	hfe_drive = drive;
	mfm_readsector(&hfe[drive].mfm, drive, sector, track, side, density);
}

static void hfe_writesector(int drive, int sector, int track, int side, int density)
{
	hfe_drive = drive;
	mfm_writesector(&hfe[drive].mfm, drive, sector, track, side, density);
}

static void hfe_readaddress(int drive, int track, int side, int density)
{
	hfe_drive = drive;
	mfm_readaddress(&hfe[drive].mfm, drive, track, side, density);
}

static void hfe_format(int drive, int track, int side, int density)
{
	hfe_drive = drive;
	mfm_format(&hfe[drive].mfm, drive, track, side, density);
}

static void hfe_stop()
{
	mfm_stop(&hfe[hfe_drive].mfm);
}

static void hfe_poll()
{
	mfm_common_poll(&hfe[hfe_drive].mfm);
}

static disc_funcs hfe_disc_funcs = {
	.seek        = hfe_seek,
	.readsector  = hfe_readsector,
	.writesector = hfe_writesector,
	.readaddress = hfe_readaddress,
	.poll        = hfe_poll,
	.format      = hfe_format,
	.stop        = hfe_stop,
	.close       = hfe_close
};

static void
hfe_mfm_savestate(FILE *f, const mfm_t *mfm)
{
	int c;

	savestate_write_rle(f, mfm->track_data, sizeof(mfm->track_data));
	for (c = 0; c < 3; c++) {
		savestate_write_i32(f, mfm->track_len[c]);
	}
	for (c = 0; c < 3; c++) {
		savestate_write_i32(f, mfm->track_index[c]);
	}
	savestate_write_i32(f, mfm->sector);
	savestate_write_i32(f, mfm->track);
	savestate_write_i32(f, mfm->side);
	savestate_write_i32(f, mfm->drive);
	savestate_write_i32(f, mfm->density);
	savestate_write_i32(f, mfm->in_read);
	savestate_write_i32(f, mfm->in_write);
	savestate_write_i32(f, mfm->in_readaddr);
	savestate_write_i32(f, mfm->in_format);
	savestate_write_i32(f, mfm->sync_required);
	savestate_write_u64(f, mfm->buffer);
	savestate_write_i32(f, mfm->pos);
	savestate_write_i32(f, mfm->revs);
	savestate_write_i32(f, mfm->indextime_blank);
	savestate_write_i32(f, mfm->pollbytesleft);
	savestate_write_i32(f, mfm->pollbitsleft);
	savestate_write_i32(f, mfm->ddidbitsleft);
	savestate_write_i32(f, mfm->readidpoll);
	savestate_write_i32(f, mfm->readdatapoll);
	savestate_write_i32(f, mfm->writedatapoll);
	savestate_write_i32(f, mfm->rw_write);
	savestate_write_i32(f, mfm->nextsector);
	savestate_write_rle(f, mfm->sectordat, sizeof(mfm->sectordat));
	savestate_write_u16(f, mfm->crc);
	savestate_write_i32(f, mfm->lastdat[0]);
	savestate_write_i32(f, mfm->lastdat[1]);
	savestate_write_i32(f, mfm->sectorcrc[0]);
	savestate_write_i32(f, mfm->sectorcrc[1]);
	savestate_write_i32(f, mfm->sectorsize);
	savestate_write_i32(f, mfm->fdc_sectorsize);
	savestate_write_i32(f, mfm->last_bit);
	savestate_write_i32(f, mfm->write_protected);
}

static void
hfe_mfm_loadstate(FILE *f, mfm_t *mfm)
{
	int c;

	savestate_read_rle(f, mfm->track_data, sizeof(mfm->track_data));
	for (c = 0; c < 3; c++) {
		mfm->track_len[c] = savestate_read_i32(f);
	}
	for (c = 0; c < 3; c++) {
		mfm->track_index[c] = savestate_read_i32(f);
	}
	mfm->sector = savestate_read_i32(f);
	mfm->track = savestate_read_i32(f);
	mfm->side = savestate_read_i32(f);
	mfm->drive = savestate_read_i32(f);
	mfm->density = savestate_read_i32(f);
	mfm->in_read = savestate_read_i32(f);
	mfm->in_write = savestate_read_i32(f);
	mfm->in_readaddr = savestate_read_i32(f);
	mfm->in_format = savestate_read_i32(f);
	mfm->sync_required = savestate_read_i32(f);
	mfm->buffer = savestate_read_u64(f);
	mfm->pos = savestate_read_i32(f);
	mfm->revs = savestate_read_i32(f);
	mfm->indextime_blank = savestate_read_i32(f);
	mfm->pollbytesleft = savestate_read_i32(f);
	mfm->pollbitsleft = savestate_read_i32(f);
	mfm->ddidbitsleft = savestate_read_i32(f);
	mfm->readidpoll = savestate_read_i32(f);
	mfm->readdatapoll = savestate_read_i32(f);
	mfm->writedatapoll = savestate_read_i32(f);
	mfm->rw_write = savestate_read_i32(f);
	mfm->nextsector = savestate_read_i32(f);
	savestate_read_rle(f, mfm->sectordat, sizeof(mfm->sectordat));
	mfm->crc = savestate_read_u16(f);
	mfm->lastdat[0] = savestate_read_i32(f);
	mfm->lastdat[1] = savestate_read_i32(f);
	mfm->sectorcrc[0] = savestate_read_i32(f);
	mfm->sectorcrc[1] = savestate_read_i32(f);
	mfm->sectorsize = savestate_read_i32(f);
	mfm->fdc_sectorsize = savestate_read_i32(f);
	mfm->last_bit = savestate_read_i32(f);
	mfm->write_protected = savestate_read_i32(f);
}

/**
 * Write the HFE floppy image state to a suspend snapshot.
 *
 * The image file handle, header and track table are not stored; they are
 * rebuilt when the image is re-opened before this state is restored. The
 * MFM decode state (including the buffered track) is stored so an
 * in-flight operation carries across; its writeback function pointer is
 * left untouched.
 */
void
hfe_savestate(FILE *f)
{
	int d;

	savestate_write_i32(f, hfe_drive);
	for (d = 0; d < 4; d++) {
		savestate_write_i32(f, hfe[d].current_track);
		savestate_write_i32(f, hfe[d].write_prot);
		hfe_mfm_savestate(f, &hfe[d].mfm);
	}
}

/**
 * Restore the HFE floppy image state from a suspend snapshot.
 */
void
hfe_loadstate(FILE *f)
{
	int d;

	hfe_drive = savestate_read_i32(f);
	for (d = 0; d < 4; d++) {
		hfe[d].current_track = savestate_read_i32(f);
		hfe[d].write_prot = savestate_read_i32(f);
		hfe_mfm_loadstate(f, &hfe[d].mfm);
	}
}
