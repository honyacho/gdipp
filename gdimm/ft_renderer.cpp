#include "stdafx.h"
#include "ft_renderer.h"
#include "freetype.h"
#include "helper_func.h"
#include "gdimm.h"

FT_F26Dot6 gdimm_ft_renderer::get_embolden(const font_setting_cache *setting_cache, char font_weight_class, char text_weight_class)
{
	// the embolden weight is based on the difference between demanded weight and the regular weight

	FT_F26Dot6 embolden = setting_cache->embolden;

	const FT_F26Dot6 embolden_values[] = {-32, -16, 0, 16, 32};
	const char embolden_class_count = sizeof(embolden_values) / sizeof(FT_F26Dot6);
	const char regular_embolden_class = (embolden_class_count - 1) / 2;

	char embolden_class = text_weight_class - font_weight_class + regular_embolden_class;

	if (embolden_class < 0)
		embolden_class = 0;
	else if (embolden_class >= embolden_class_count)
		embolden_class = embolden_class_count - 1;

	embolden += embolden_values[embolden_class];

	return embolden;
}

void gdimm_ft_renderer::get_font_size(const OUTLINETEXTMETRICW *outline_metrics, FT_Short xAvgCharWidth, FT_UInt &font_width, FT_UInt &font_height)
{
	/*
	while the height in FreeType scaler has the same meaning as the height value in LOGFONT structure, the width is different
	what we know is, when the width in LOGFONT is the xAvgCharWidth (from the OS/2 table), the corresponding FreeType scaler width is the height
	therefore we need conversion when LOGFONT width is not 0
	simple calculation yields freetype_width = logfont_width * em_square / xAvgCharWidth
	note that the tmAveCharWidth field in TEXTMETRIC is the actual LOGFONT width, which is never 0
	*/

	assert(outline_metrics != NULL);

	font_height = outline_metrics->otmTextMetrics.tmHeight - outline_metrics->otmTextMetrics.tmInternalLeading;

	if (xAvgCharWidth == 0)
		font_width = font_height * outline_metrics->otmTextMetrics.tmDigitizedAspectX / outline_metrics->otmTextMetrics.tmDigitizedAspectY;
	else
	{
		// compare the xAvgCharWidth against the current average char width
		font_width = outline_metrics->otmTextMetrics.tmAveCharWidth * outline_metrics->otmEMSquare / xAvgCharWidth;
	}
}

FT_ULong gdimm_ft_renderer::get_load_flags(const font_setting_cache *setting_cache, FT_Render_Mode render_mode)
{
	FT_ULong load_flags = FT_LOAD_CROP_BITMAP | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH |
		(setting_cache->embedded_bitmap ? 0 : FT_LOAD_NO_BITMAP);

	if (setting_cache->hinting == 0)
		load_flags |= FT_LOAD_NO_HINTING;
	else
	{
		switch (setting_cache->hinting)
		{
		case 1:
			load_flags |= FT_LOAD_TARGET_LIGHT;
			break;
		case 3:
			load_flags |= FT_LOAD_TARGET_MONO;
			break;
		default:
			{
				if (render_mode == FT_RENDER_MODE_LCD)
					load_flags |= FT_LOAD_TARGET_LCD;
				else
					load_flags |= FT_LOAD_TARGET_NORMAL;
				break;
			}
		}

		switch (setting_cache->auto_hinting)
		{
		case 0:
			load_flags |= FT_LOAD_NO_AUTOHINT;
			break;
		case 2:
			load_flags |= FT_LOAD_FORCE_AUTOHINT;
			break;
		default:
			load_flags |= FT_LOAD_DEFAULT;
			break;
		}
	}

	return load_flags;
}

void gdimm_ft_renderer::oblique_outline(const FT_Outline *outline, double slant_adv)
{
	// advancement of slant on x-axis
	FT_Matrix oblique_mat = {to_16dot16(1), to_16dot16(slant_adv), 0, to_16dot16(1)};
	FT_Outline_Transform(outline, &oblique_mat);
}

bool gdimm_ft_renderer::generate_outline_glyph(FT_Glyph *glyph,
	WORD glyph_index,
	const FTC_Scaler scaler,
	FT_F26Dot6 embolden,
	FT_ULong load_flags,
	bool is_italic) const
{
	FT_Error ft_error;

	FT_Glyph cached_glyph;

	{
		// the FreeType function seems not thread-safe
		gdimm_lock lock(LOCK_FREETYPE);
		ft_error = FTC_ImageCache_LookupScaler(ft_glyph_cache, scaler, load_flags, glyph_index, &cached_glyph, NULL);
		if (ft_error != 0)
			return NULL;
	}

	// some fonts are embedded with pre-rendered glyph bitmap
	// in that case, use original ExtTextOutW
	if (cached_glyph->format != FT_GLYPH_FORMAT_OUTLINE)
		return NULL;

	// if italic style is demanded, and the font has italic glyph, do oblique transformation
	const bool is_oblique = ((_context->outline_metrics->otmTextMetrics.tmItalic != 0) && !is_italic);
	const bool need_embolden = (embolden != 0);
	const bool need_glyph_copy = (is_oblique || need_embolden);

	if (need_glyph_copy)
	{
		FT_Glyph_Copy(cached_glyph, glyph);
		FT_Outline *glyph_outline = &(reinterpret_cast<FT_OutlineGlyph>(*glyph)->outline);

		// it seems faster if oblique first, and then embolden
		if (is_oblique)
			oblique_outline(glyph_outline, 0.3);

		if (need_embolden)
		{
			ft_error = FT_Outline_Embolden(glyph_outline, embolden);
			assert(ft_error == 0);
		}
	}
	else
		*glyph = cached_glyph;

	return need_glyph_copy;
}

const FT_Glyph gdimm_ft_renderer::generate_bitmap_glyph(WORD glyph_index,
	const FTC_Scaler scaler,
	FT_Render_Mode render_mode,
	FT_F26Dot6 embolden,
	FT_ULong load_flags,
	bool is_italic,
	bool request_outline,
	uint64_t font_trait) const
{
	FT_Error ft_error;

	FT_Glyph glyph;

	if (request_outline)
	{
		generate_outline_glyph(&glyph, glyph_index, scaler, embolden, load_flags, is_italic);
		return glyph;
	}

	glyph = _glyph_cache.lookup_glyph(font_trait, glyph_index, true);
	if (glyph == NULL)
	{
		// double-check lock
		gdimm_lock lock(LOCK_GLYPH_CACHE);
		glyph = _glyph_cache.lookup_glyph(font_trait, glyph_index, true);
		if (glyph == NULL)
		{
			// no cached glyph, or outline glyph is requested, generate outline
			const bool local_glyph = generate_outline_glyph(&glyph, glyph_index, scaler, embolden, load_flags, is_italic);

			// outline -> bitmap conversion
			{
				// the FreeType function seems not thread-safe
				gdimm_lock lock(LOCK_FREETYPE);
				ft_error = FT_Glyph_To_Bitmap(&glyph, render_mode, NULL, local_glyph);
				if (ft_error != 0)
					return NULL;
			}

			_glyph_cache.store_glyph(font_trait, glyph_index, true, glyph);
		}
	}

	return glyph;
}

bool gdimm_ft_renderer::generate_glyph_run(bool is_glyph_index, LPCWSTR lpString, UINT c, glyph_run &new_glyph_run, bool request_outline)
{
	gdimm_font_man font_man;

	wstring curr_font_face = metric_face_name(_context->outline_metrics);
	const wchar_t *dc_font_family = metric_family_name(_context->outline_metrics);
	const font_setting_cache *curr_setting_cache = _context->setting_cache;
	uint64_t curr_font_trait = _font_trait;

	long font_id = font_man.register_font(_context->hdc, curr_font_face.c_str());
	if (font_id < 0)
		return false;

	const gdimm_os2_metrics *os2_metrics = font_man.lookup_os2_metrics(font_id);

	FTC_ScalerRec scaler = {};
	scaler.face_id = reinterpret_cast<FTC_FaceID>(font_id);
	scaler.pixel = 1;
	get_font_size(_context->outline_metrics, (_context->log_font.lfWidth == 0 ? 0 : os2_metrics->get_xAvgCharWidth()), scaler.height, scaler.width);

	FT_F26Dot6 curr_embolden = 0;
	if (_context->log_font.lfWeight != FW_DONTCARE)
	{
		// embolden if some weight is demanded
		curr_embolden = get_embolden(curr_setting_cache, os2_metrics->get_weight_class(), static_cast<char>(_context->log_font.lfWeight));
	}

	FT_ULong curr_load_flags = get_load_flags(curr_setting_cache, _render_mode);

	if (is_glyph_index)
	{
		// directly render glyph indices with the current DC font

		for (UINT i = 0; i < c; i++)
		{
			glyph_node new_glyph = {};
			new_glyph.glyph = generate_bitmap_glyph(lpString[i],
				&scaler,
				_render_mode,
				curr_embolden,
				curr_load_flags,
				os2_metrics->is_italic(),
				request_outline,
				curr_font_trait);

			if (new_glyph.glyph == NULL)
			{
				if (request_outline)
					return false;
			}
			else if (curr_setting_cache->kerning && i > 0 && !request_outline)
			{
				new_glyph.bbox.left = font_man.lookup_kern(&scaler, lpString[i-1], lpString[i]);
				new_glyph.bbox.right = new_glyph.bbox.left;
			}
			
			new_glyph_run.push_back(new_glyph);
		}
	}
	else
	{
		FT_Render_Mode curr_render_mode = _render_mode;

		UINT rendered_count = 0;
		int font_link_index = 0;
		wstring final_string(lpString, c);
		wstring glyph_indices(L"", c);
		new_glyph_run.resize(c);

		while (true)
		{
			font_man.get_glyph_indices(reinterpret_cast<long>(scaler.face_id), &final_string[0], c, &glyph_indices[0]);

			glyph_run::iterator iter = new_glyph_run.begin();

			for (UINT i = 0; i < c; i++, iter++)
			{
				if (final_string[i] == L'\0')
					continue;

				// do not render control characters, even the corresponding glyphs exist in font
				if (iswcntrl(final_string[i]) && !request_outline)
					iter->glyph = NULL;
				else if (glyph_indices[i] != 0xffff)
				{
					iter->glyph = generate_bitmap_glyph(glyph_indices[i],
						&scaler,
						curr_render_mode,
						curr_embolden,
						curr_load_flags, 
						os2_metrics->is_italic(),
						request_outline,
						curr_font_trait);

					if (iter->glyph == NULL)
					{
						if (request_outline)
							return false;
					}
					else if (curr_setting_cache->kerning && i > 0 && !request_outline)
					{
						iter->bbox.left = font_man.lookup_kern(&scaler, glyph_indices[i-1], glyph_indices[i]);
						iter->bbox.right = iter->bbox.left;
					}
				}
				else
					continue;
					
				final_string[i] = L'\0';
				rendered_count += 1;
			}

			if (rendered_count >= c)
			{
				assert(rendered_count == c);
				break;
			}

			// font linking

			const font_link_node *curr_link = font_link_instance.lookup_link(dc_font_family, font_link_index);
			if (curr_link == NULL)
				return false;
			font_link_index += 1;
			
			LOGFONTW linked_log_font = _context->log_font;
			/*
			this reset is essential to make GetGlyphIndices work correctly
			for example, lfOutPrecision might be OUT_PS_ONLY_PRECIS for Myriad Pro
			if create HFONT of Microsoft YaHei with such lfOutPrecision, GetGlyphIndices always fails
			*/
			linked_log_font.lfOutPrecision = OUT_DEFAULT_PRECIS;
			wcsncpy_s(linked_log_font.lfFaceName, curr_link->font_family.c_str(), LF_FACESIZE);

			font_id = font_man.link_font(linked_log_font, curr_font_face);
			assert(font_id < 0);

			// reload metrics for the linked font

			scaler.face_id = reinterpret_cast<FTC_FaceID>(font_id);

			if (curr_link->scaling != 1.0)
			{
				// apply font linking scaling factor
				scaler.width = static_cast<FT_UInt>(scaler.width * curr_link->scaling);
				scaler.height = static_cast<FT_UInt>(scaler.height * curr_link->scaling);
			}

			os2_metrics = font_man.lookup_os2_metrics(font_id);
			
			const gdimm_setting_trait setting_trait = {curr_font_face.c_str(), os2_metrics->get_weight_class(), os2_metrics->is_italic()};
			curr_setting_cache = setting_cache_instance.lookup(&setting_trait);

			if (!get_render_mode(curr_setting_cache, _context->bmp_header.biBitCount, _context->log_font.lfQuality, curr_render_mode))
				return false;

			curr_font_trait = get_font_trait(linked_log_font, curr_render_mode);

			curr_embolden = 0;
			if (linked_log_font.lfWeight != FW_DONTCARE)
				curr_embolden = get_embolden(curr_setting_cache, setting_trait.weight_class, static_cast<char>(linked_log_font.lfWeight));

			curr_load_flags = get_load_flags(curr_setting_cache, _render_mode);
		}
	}

	return true;
}

int gdimm_ft_renderer::render(bool is_glyph_index, bool is_pdy, LPCWSTR lpString, UINT c, CONST INT *lpDx, glyph_run &new_glyph_run)
{
	bool b_ret;

	b_ret = generate_glyph_run(is_glyph_index, lpString, c, new_glyph_run, false);
	if (!b_ret)
		return 0;

	int glyph_run_height = 0;
	POINT pen_pos = {};

	for (glyph_run::iterator iter = new_glyph_run.begin(); iter != new_glyph_run.end(); iter++)
	{
		iter->bbox.left += pen_pos.x;
		iter->bbox.top = pen_pos.y;

		FT_Vector glyph_advancement = {};
		if (iter->glyph != NULL)
		{
			glyph_advancement = iter->glyph->advance;
			glyph_run_height = max(glyph_run_height, reinterpret_cast<FT_BitmapGlyph>(iter->glyph)->bitmap.rows);
		}

		pen_pos.x += from_16dot16(glyph_advancement.x) + _char_extra;
		pen_pos.y += from_16dot16(glyph_advancement.y);

		iter->bbox.right += pen_pos.x;
		iter->bbox.bottom = pen_pos.y;
	}

	return glyph_run_height;
}

bool gdimm_ft_renderer::get_glyph_metrics(wchar_t glyph_char, bool is_glyph_index, LPGLYPHMETRICS lpgm)
{
	bool b_ret;
	FT_Error ft_error;

	glyph_run new_glyph_run;
	b_ret = generate_glyph_run(is_glyph_index, &glyph_char, 1, new_glyph_run, true);
	if (b_ret)
	{
		assert(new_glyph_run.size() == 1);

		const FT_Glyph target_glyph = new_glyph_run.front().glyph;

		FT_BBox glyph_bbox;
		ft_error = FT_Outline_Get_BBox(&(reinterpret_cast<FT_OutlineGlyph>(target_glyph)->outline), &glyph_bbox);
		assert(ft_error == 0);

		lpgm->gmBlackBoxX = from_26dot6(glyph_bbox.xMax - glyph_bbox.xMin);
		lpgm->gmBlackBoxY = from_26dot6(glyph_bbox.yMax - glyph_bbox.yMin);
		lpgm->gmptGlyphOrigin.x = from_26dot6(glyph_bbox.xMin);
		lpgm->gmptGlyphOrigin.y = from_26dot6(glyph_bbox.yMax);
		lpgm->gmCellIncX = static_cast<short>(from_16dot16(target_glyph->advance.x));
		lpgm->gmCellIncY = static_cast<short>(from_16dot16(target_glyph->advance.y));
	}

	return b_ret;
}