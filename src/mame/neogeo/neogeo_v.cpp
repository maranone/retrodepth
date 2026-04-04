// license:BSD-3-Clause
// copyright-holders:Bryan McPhail,Ernesto Corvi,Andrew Prime,Zsolt Vasvari
// thanks-to:Fuzz
/***************************************************************************

    Neo-Geo hardware

****************************************************************************/

#include "emu.h"
#include "neogeo.h"
#include "video/resnet.h"
#include "retrodepth.h"
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>

#define VERBOSE     (0)


/*************************************
 *
 *  Palette handling
 *
 *************************************/

void neogeo_base_state::create_rgb_lookups()
{
	static const int resistances[] = {3900, 2200, 1000, 470, 220};

	/* compute four sets of weights - with or without the pulldowns -
	   ensuring that we use the same scaler for all */
	double weights_normal[5];
	double scaler = compute_resistor_weights(0, 255, -1,
											5, resistances, weights_normal, 0, 0,
											0, nullptr, nullptr, 0, 0,
											0, nullptr, nullptr, 0, 0);

	double weights_dark[5];
	compute_resistor_weights(0, 255, scaler,
							5, resistances, weights_dark, 8200, 0,
							0, nullptr, nullptr, 0, 0,
							0, nullptr, nullptr, 0, 0);

	double weights_shadow[5];
	compute_resistor_weights(0, 255, scaler,
							5, resistances, weights_shadow, 150, 0,
							0, nullptr, nullptr, 0, 0,
							0, nullptr, nullptr, 0, 0);

	double weights_dark_shadow[5];
	compute_resistor_weights(0, 255, scaler,
							5, resistances, weights_dark_shadow, 1.0 / ((1.0 / 8200) + (1.0 / 150)), 0,
							0, nullptr, nullptr, 0, 0,
							0, nullptr, nullptr, 0, 0);

	for (int i = 0; i < 32; i++)
	{
		int const i4 = BIT(i, 4);
		int const i3 = BIT(i, 3);
		int const i2 = BIT(i, 2);
		int const i1 = BIT(i, 1);
		int const i0 = BIT(i, 0);
		m_palette_lookup[i][0] = combine_weights(weights_normal, i0, i1, i2, i3, i4);
		m_palette_lookup[i][1] = combine_weights(weights_dark, i0, i1, i2, i3, i4);
		m_palette_lookup[i][2] = combine_weights(weights_shadow, i0, i1, i2, i3, i4);
		m_palette_lookup[i][3] = combine_weights(weights_dark_shadow, i0, i1, i2, i3, i4);
	}
}

void neogeo_base_state::set_pens()
{
	const pen_t *pen_base = m_palette->pens() + m_palette_bank + (m_screen_shadow ? 0x2000 : 0);
	m_sprgen->set_pens(pen_base);
	m_bg_pen = pen_base + 0xfff;
}


void neogeo_base_state::set_screen_shadow(int state)
{
	m_screen_shadow = state;
	set_pens();
}


void neogeo_base_state::set_palette_bank(int state)
{
	m_palette_bank = state ? 0x1000 : 0;
	set_pens();
}


uint16_t neogeo_base_state::paletteram_r(offs_t offset)
{
	return m_paletteram[m_palette_bank + offset];
}


void neogeo_base_state::paletteram_w(offs_t offset, uint16_t data)
{
	offset += m_palette_bank;
	m_paletteram[offset] = data;

	uint8_t const dark = data >> 15;
	uint8_t const r = ((data >> 14) & 0x1) | ((data >> 7) & 0x1e);
	uint8_t const g = ((data >> 13) & 0x1) | ((data >> 3) & 0x1e);
	uint8_t const b = ((data >> 12) & 0x1) | ((data << 1) & 0x1e);

	m_palette->set_pen_color(offset,
								m_palette_lookup[r][dark],
								m_palette_lookup[g][dark],
								m_palette_lookup[b][dark]); // normal

	m_palette->set_pen_color(offset + 0x2000,
								m_palette_lookup[r][dark+2],
								m_palette_lookup[g][dark+2],
								m_palette_lookup[b][dark+2]); // shadow
}


/*************************************
 *
 *  Video system start
 *
 *************************************/

void neogeo_base_state::video_start()
{
	create_rgb_lookups();

	m_paletteram.resize(0x1000 * 2, 0);

	m_screen_shadow = false;
	m_palette_bank = 0;

	save_item(NAME(m_paletteram));
	save_item(NAME(m_screen_shadow));
	save_item(NAME(m_palette_bank));

	set_pens();
}


/*************************************
 *
 *  Video system reset
 *
 *************************************/

void neogeo_base_state::video_reset()
{
}


/*************************************
 *
 *  Video update
 *
 *************************************/

uint32_t neogeo_base_state::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	// fill with background color first
	bitmap.fill(*m_bg_pen, cliprect);

	m_sprgen->draw_sprites(bitmap, cliprect.min_y);

	m_sprgen->draw_fixed_layer(bitmap, cliprect.min_y);

	// --- RetroDepth layer export ---
	// 4 sprite groups routed by palette index via RDPaletteRoute control shmem.
	// route[pal] = 0-3 selects grp0-grp3; 0xFF defaults to grp0.
	if (retrodepth_active())
	{
		static constexpr int N_GROUPS = 4;

		// Read palette→group routing table from retrodepth
		RDPaletteRoute ctrl;
		retrodepth_read_palette_route(&ctrl);

		const rectangle& vis = screen.visible_area();

		static bitmap_rgb32 s_bg, s_grp[N_GROUPS], s_fix;
		const int bw = bitmap.width(), bh = bitmap.height();
		if (s_bg.width() != bw || s_bg.height() != bh) {
			s_bg.allocate(bw, bh);
			for (int g = 0; g < N_GROUPS; ++g) s_grp[g].allocate(bw, bh);
			s_fix.allocate(bw, bh);
		}

		s_bg.fill(*m_bg_pen, cliprect);
		for (int g = 0; g < N_GROUPS; ++g) {
			s_grp[g].fill(1, cliprect); // sentinel: unfilled pixel != real black
			m_sprgen->draw_sprites(s_grp[g], cliprect.min_y,
			                       0, 95, 0, 255, ctrl.route, (uint8_t)g);
		}
		s_fix.fill(1, cliprect);
		m_sprgen->draw_fixed_layer(s_fix, cliprect.min_y);

		// At end of frame: write palette data + layers to shared memory.
		if (cliprect.max_y >= vis.max_y)
		{
			// Ship palette colours so retrodepth can show swatches.
			const pen_t* pens = m_palette->pens() + m_palette_bank;
			retrodepth_write_palette_data(reinterpret_cast<const uint32_t*>(pens), 256 * 16);

			const int w = vis.width(), h = vis.height();
			std::vector<uint32_t> packed(w * h);
			std::vector<uint16_t> owners(w * h, RD_OWNER_NONE);
			std::unordered_map<uint32_t, uint16_t> owner_maps[N_GROUPS];
			std::unordered_map<uint32_t, uint16_t> fix_owner_map;
			if (ctrl.thumb_requested)
			{
				for (int p = 0; p < 256; ++p)
				{
					uint8_t g = ctrl.route[p];
					if (g == 0xFF) g = 0;
					for (int c = 0; c < 16; ++c) {
						uint32_t col = reinterpret_cast<const uint32_t*>(pens)[p * 16 + c];
						if (!owner_maps[g].count(col))
							owner_maps[g][col] = (uint16_t)p;
						if (!fix_owner_map.count(col))
							fix_owner_map[col] = (uint16_t)p;
					}
				}
			}

			auto write_layer = [&](uint32_t z_order, const char* name, bitmap_rgb32& bmp,
			                       const std::unordered_map<uint32_t, uint16_t>* owner_map) {
				for (int row = 0; row < h; ++row)
					memcpy(&packed[row * w],
					       &bmp.pix(vis.min_y + row, vis.min_x),
					       w * sizeof(uint32_t));
				if (owner_map) {
					for (int i = 0; i < w * h; ++i) {
						auto it = owner_map->find(packed[i]);
						owners[i] = (it != owner_map->end()) ? it->second : RD_OWNER_NONE;
					}
					retrodepth_write_layer(z_order, name, packed.data(), owners.data(), w, h);
				} else {
					retrodepth_write_layer(z_order, name, packed.data(), nullptr, w, h);
				}
			};

			static const char* k_grp_names[N_GROUPS] = { "grp0", "grp1", "grp2", "grp3" };
			write_layer(0, "background", s_bg, nullptr);
			for (int g = 0; g < N_GROUPS; ++g)
				write_layer(g + 1, k_grp_names[g], s_grp[g], ctrl.thumb_requested ? &owner_maps[g] : nullptr);
			write_layer(N_GROUPS + 1, "fix", s_fix, ctrl.thumb_requested ? &fix_owner_map : nullptr);

			// Per-palette thumbnails — only when the editor is open.
			// Renders 2 palettes per frame into a 32×32 downsampled view.
			// Full 64-palette cycle completes in 32 frames (~0.5 s).
			if (ctrl.thumb_requested)
			{
				static bitmap_rgb32 s_tbmp;
				static uint8_t s_troute[256];
				static int s_tpal = 0;
				constexpr int TD = (int)RD_THUMB_DIM;
				constexpr int PER_FRAME = 2;

				if (s_tbmp.width() < bw || s_tbmp.height() < bh)
					s_tbmp.allocate(bw, bh);

				const int sw = vis.width(), sh = vis.height();
				for (int t = 0; t < PER_FRAME; ++t, s_tpal = (s_tpal + 1) % 64)
				{
					memset(s_troute, 0xFF, 256);
					s_troute[s_tpal] = 0;
					s_tbmp.fill(0, cliprect);
					m_sprgen->draw_sprites(s_tbmp, cliprect.min_y,
					                       0, 95, 0, 255, s_troute, 0);

					uint32_t thumb[TD * TD];
					for (int ty = 0; ty < TD; ++ty)
						for (int tx = 0; tx < TD; ++tx)
							thumb[ty * TD + tx] = s_tbmp.pix(
							    vis.min_y + ty * sh / TD,
							    vis.min_x + tx * sw / TD);

					retrodepth_write_palette_thumbs(thumb, (uint32_t)s_tpal);
				}
			}

			retrodepth_commit();
			// Debug slow-motion: sleep after each frame commit so each frame stays
			// visible long enough to inspect. Set SLOWMO_MS to 0 to disable.
			static constexpr int SLOWMO_MS = 0;
			if constexpr (SLOWMO_MS > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(SLOWMO_MS));
		}
	}

	return 0;
}


/*************************************
 *
 *  Video control
 *
 *************************************/

uint16_t neogeo_base_state::get_video_control()
{
	/*
	    The format of this very important location is:  AAAA AAAA A??? BCCC

	    A is the raster line counter. mosyougi relies solely on this to do the
	      raster effects on the title screen; sdodgeb loops waiting for the top
	      bit to be 1; zedblade heavily depends on it to work correctly (it
	      checks the top bit in the IRQ2 handler).
	    B is definitely a PAL/NTSC flag. (LSPC2 only) Evidence:
	      1) trally changes the position of the speed indicator depending on
	         it (0 = lower 1 = higher).
	      2) samsho3 sets a variable to 60 when the bit is 0 and 50 when it's 1.
	         This is obviously the video refresh rate in Hz.
	      3) samsho3 sets another variable to 256 or 307. This could be the total
	         screen height (including vblank), or close to that.
	      Some games (e.g. lstbld2, samsho3) do this (or similar):
	      bclr    #$0, $3c000e.l
	      when the bit is set, so 3c000e (whose function is unknown) has to be
	      related
	    C animation counter lower 3 bits
	*/

	// the vertical counter chain goes from 0xf8 - 0x1ff
	uint16_t v_counter = m_screen->vpos() + 0x100;

	if (v_counter >= 0x200)
		v_counter = v_counter - NEOGEO_VTOTAL;

	uint16_t const ret = (v_counter << 7) | (m_sprgen->get_auto_animation_counter() & 0x0007);

	if (!machine().side_effects_disabled())
	{
		if (VERBOSE)
			logerror("%s: video_control read (%04x)\n", machine().describe_context(), ret);
	}

	return ret;
}


void neogeo_base_state::set_video_control(uint16_t data)
{
	if (VERBOSE) logerror("%s: video control write %04x\n", machine().describe_context(), data);

	m_sprgen->set_auto_animation_speed(data >> 8);
	m_sprgen->set_auto_animation_disabled(BIT(data, 3));

	set_display_position_interrupt_control(data & 0x00f0);
}


uint16_t neogeo_base_state::video_register_r(address_space &space, offs_t offset, uint16_t mem_mask)
{
	uint16_t ret;

	// accessing the LSB only is not mapped
	if (mem_mask == 0x00ff)
		ret = unmapped_r(space) & 0x00ff;
	else
	{
		switch (offset)
		{
		default:
		case 0x00:
		case 0x01: ret = m_sprgen->get_videoram_data(); break;
		case 0x02: ret = m_sprgen->get_videoram_modulo(); break;
		case 0x03: ret = get_video_control(); break;
		}
	}

	return ret;
}


void neogeo_base_state::video_register_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	// accessing the LSB only is not mapped
	if (mem_mask != 0x00ff)
	{
		// accessing the MSB only stores same data in MSB and LSB
		if (mem_mask == 0xff00)
			data = (data & 0xff00) | (data >> 8);

		switch (offset)
		{
		case 0x00: m_sprgen->set_videoram_offset(data); break;
		case 0x01: m_sprgen->set_videoram_data(data); break;
		case 0x02: m_sprgen->set_videoram_modulo(data); break;
		case 0x03: set_video_control(data); break;
		case 0x04: set_display_counter_msb(data); break;
		case 0x05: set_display_counter_lsb(data); break;
		case 0x06: acknowledge_interrupt(data); break;
		case 0x07: break; // d0: pause timer for 32 lines when in PAL mode (LSPC2 only)
		}
	}
}
