#define _WIN32_WINNT 0x500
#include "imw32.h"
#define STRICT
#include <windows.h>
#include "imext.h"

/*
=head1 NAME

win32.c - implements some win32 specific code, specifically Win32 font support.

=head1 SYNOPSIS

   int bbox[6];
   if (i_wf_bbox(facename, size, text, text_len, bbox)) {
     // we have the bbox
   }
   i_wf_text(face, im, tx, ty, cl, size, text, len, align, aa, utf8);
   i_wf_cp(face, im, tx, ty, channel, size, text, len, align, aa, utf8)

=head1 DESCRIPTION

An Imager interface to font output using the Win32 GDI.

=over

=cut
*/

#define fixed(x) ((x).value + ((x).fract) / 65536.0)

static void set_logfont(const char *face, int size, LOGFONT *lf);

static LPVOID render_text(const char *face, int size, const char *text, int length, int aa,
                          HBITMAP *pbm, SIZE *psz, TEXTMETRIC *tm, int *bbox, int utf8);
static LPWSTR utf8_to_wide_string(char const *text, int text_len, int *wide_chars);

/*
=item i_wf_bbox(face, size, text, length, bbox, utf8)

Calculate a bounding box for the text.

=cut
*/

int i_wf_bbox(const char *face, int size, const char *text, int length, int *bbox,
	      int utf8) {
  LOGFONT lf;
  HFONT font, oldFont;
  HDC dc;
  SIZE sz;
  TEXTMETRIC tm;
  ABC first, last;
  GLYPHMETRICS gm;
  MAT2 mat;
  int ascent, descent, max_ascent = -size, min_descent = size;
  const char *workp;
  int work_len;
  int got_first_ch = 0;
  unsigned long first_ch, last_ch;

  mm_log((1, "i_wf_bbox(face %s, size %d, text %p, length %d, bbox %p, utf8 %d)\n", face, size, text, length, bbox, utf8));

  set_logfont(face, size, &lf);
  font = CreateFontIndirect(&lf);
  if (!font) 
    return 0;
  dc = GetDC(NULL);
  oldFont = (HFONT)SelectObject(dc, font);

  {
    char facename[100];
    if (GetTextFace(dc, sizeof(facename), facename)) {
      mm_log((1, "  face: %s\n", facename));
    }
  }

  workp = text;
  work_len = length;
  while (work_len > 0) {
    unsigned long c;
    unsigned char cp;

    if (utf8) {
      c = i_utf8_advance(&workp, &work_len);
      if (c == ~0UL) {
        i_push_error(0, "invalid UTF8 character");
        return 0;
      }
    }
    else {
      c = (unsigned char)*workp++;
      --work_len;
    }
    if (!got_first_ch) {
      first_ch = c;
      ++got_first_ch;
    }
    last_ch = c;

    cp = c > '~' ? '.' : c < ' ' ? '.' : c;
    
    memset(&mat, 0, sizeof(mat));
    mat.eM11.value = 1;
    mat.eM22.value = 1;
    if (GetGlyphOutline(dc, (UINT)c, GGO_METRICS, &gm, 0, NULL, &mat) != GDI_ERROR) {
      mm_log((2, "  glyph '%c' (%02x): bbx (%u,%u) org (%d,%d) inc(%d,%d)\n",
	      cp, c, gm.gmBlackBoxX, gm.gmBlackBoxY, gm.gmptGlyphOrigin.x,
		gm.gmptGlyphOrigin.y, gm.gmCellIncX, gm.gmCellIncY));

      ascent = gm.gmptGlyphOrigin.y;
      descent = ascent - gm.gmBlackBoxY;
      if (ascent > max_ascent) max_ascent = ascent;
      if (descent < min_descent) min_descent = descent;
    }
    else {
	mm_log((1, "  glyph '%c' (%02x): error %d\n", cp, c, GetLastError()));
    }
  }

  if (utf8) {
    int wide_chars;
    LPWSTR wide_text = utf8_to_wide_string(text, length, &wide_chars);

    if (!wide_text)
      return 0;

    if (!GetTextExtentPoint32W(dc, wide_text, wide_chars, &sz)
	|| !GetTextMetrics(dc, &tm)) {
      SelectObject(dc, oldFont);
      ReleaseDC(NULL, dc);
      DeleteObject(font);
      return 0;
    }

    myfree(wide_text);
  }
  else {
    if (!GetTextExtentPoint32(dc, text, length, &sz)
	|| !GetTextMetrics(dc, &tm)) {
      SelectObject(dc, oldFont);
      ReleaseDC(NULL, dc);
      DeleteObject(font);
      return 0;
    }
  }
  bbox[BBOX_GLOBAL_DESCENT] = -tm.tmDescent;
  bbox[BBOX_DESCENT] = min_descent == size ? -tm.tmDescent : min_descent;
  bbox[BBOX_POS_WIDTH] = sz.cx;
  bbox[BBOX_ADVANCE_WIDTH] = sz.cx;
  bbox[BBOX_GLOBAL_ASCENT] = tm.tmAscent;
  bbox[BBOX_ASCENT] = max_ascent == -size ? tm.tmAscent : max_ascent;
  
  if (length
      && GetCharABCWidths(dc, first_ch, first_ch, &first)
      && GetCharABCWidths(dc, last_ch, last_ch, &last)) {
    mm_log((1, "first: %d A: %d  B: %d  C: %d\n", first_ch,
	    first.abcA, first.abcB, first.abcC));
    mm_log((1, "last: %d A: %d  B: %d  C: %d\n", last_ch,
	    last.abcA, last.abcB, last.abcC));
    bbox[BBOX_NEG_WIDTH] = first.abcA;
    bbox[BBOX_RIGHT_BEARING] = last.abcC;
    if (last.abcC < 0)
      bbox[BBOX_POS_WIDTH] -= last.abcC;
  }
  else {
    bbox[BBOX_NEG_WIDTH] = 0;
    bbox[BBOX_RIGHT_BEARING] = 0;
  }

  SelectObject(dc, oldFont);
  ReleaseDC(NULL, dc);
  DeleteObject(font);

  mm_log((1, " bbox=> negw=%d glob_desc=%d pos_wid=%d glob_asc=%d desc=%d asc=%d adv_width=%d rightb=%d\n", bbox[0], bbox[1], bbox[2], bbox[3], bbox[4], bbox[5], bbox[6], bbox[7]));

  return BBOX_RIGHT_BEARING + 1;
}

/*
=item i_wf_text(face, im, tx, ty, cl, size, text, len, align, aa)

Draws the text in the given color.

=cut
*/

int
i_wf_text(const char *face, i_img *im, int tx, int ty, const i_color *cl, int size, 
	  const char *text, int len, int align, int aa, int utf8) {
  unsigned char *bits;
  HBITMAP bm;
  SIZE sz;
  int line_width;
  int x, y;
  int ch;
  TEXTMETRIC tm;
  int top;
  int bbox[BOUNDING_BOX_COUNT];

  mm_log((1, "i_wf_text(face %s, im %p, tx %d, ty %d, cl %p, size %d, text %p, length %d, align %d, aa %d,  utf8 %d)\n", face, im, tx, ty, cl, size, text, len, align, aa, aa, utf8));

  if (!i_wf_bbox(face, size, text, len, bbox, utf8))
    return 0;

  bits = render_text(face, size, text, len, aa, &bm, &sz, &tm, bbox, utf8);
  if (!bits)
    return 0;
  
  tx += bbox[BBOX_NEG_WIDTH];
  line_width = sz.cx * 3;
  line_width = (line_width + 3) / 4 * 4;
  top = ty;
  if (align) {
    top -= tm.tmAscent;
  }
  else {
    top -= tm.tmAscent - bbox[BBOX_ASCENT];
  }

  for (y = 0; y < sz.cy; ++y) {
    for (x = 0; x < sz.cx; ++x) {
      i_color pel;
      int scale = bits[3 * x];
      i_gpix(im, tx+x, top+sz.cy-y-1, &pel);
      for (ch = 0; ch < im->channels; ++ch) {
	pel.channel[ch] = 
	  ((255-scale) * pel.channel[ch] + scale*cl->channel[ch]) / 255;
      }
      i_ppix(im, tx+x, top+sz.cy-y-1, &pel);
    }
    bits += line_width;
  }
  DeleteObject(bm);

  return 1;
}

/*
=item i_wf_cp(face, im, tx, ty, channel, size, text, len, align, aa)

Draws the text in the given channel.

=cut
*/

int
i_wf_cp(const char *face, i_img *im, int tx, int ty, int channel, int size, 
	  const char *text, int len, int align, int aa, int utf8) {
  unsigned char *bits;
  HBITMAP bm;
  SIZE sz;
  int line_width;
  int x, y;
  TEXTMETRIC tm;
  int top;
  int bbox[BOUNDING_BOX_COUNT];

  mm_log((1, "i_wf_cp(face %s, im %p, tx %d, ty %d, channel %d, size %d, text %p, length %d, align %d, aa %d,  utf8 %d)\n", face, im, tx, ty, channel, size, text, len, align, aa, aa, utf8));

  if (!i_wf_bbox(face, size, text, len, bbox, utf8))
    return 0;

  bits = render_text(face, size, text, len, aa, &bm, &sz, &tm, bbox, utf8);
  if (!bits)
    return 0;
  
  line_width = sz.cx * 3;
  line_width = (line_width + 3) / 4 * 4;
  top = ty;
  if (align) {
    top -= tm.tmAscent;
  }
  else {
    top -= tm.tmAscent - bbox[BBOX_ASCENT];
  }

  for (y = 0; y < sz.cy; ++y) {
    for (x = 0; x < sz.cx; ++x) {
      i_color pel;
      int scale = bits[3 * x];
      i_gpix(im, tx+x, top+sz.cy-y-1, &pel);
      pel.channel[channel] = scale;
      i_ppix(im, tx+x, top+sz.cy-y-1, &pel);
    }
    bits += line_width;
  }
  DeleteObject(bm);

  return 1;
}

static HMODULE gdi_dll;
typedef BOOL (CALLBACK *AddFontResourceExA_t)(LPCSTR, DWORD, PVOID);
static AddFontResourceExA_t AddFontResourceExAp;
typedef BOOL (CALLBACK *RemoveFontResourceExA_t)(LPCSTR, DWORD, PVOID);
static RemoveFontResourceExA_t RemoveFontResourceExAp;

/*
=item i_wf_addfont(char const *filename, char const *resource_file)

Adds a TTF font file as usable by the application.

Where possible the font is added as private to the application.

Under Windows 95/98/ME the font is added globally, since we don't have
any choice.  In either case call i_wf_delfont() to remove it.

=cut
 */
int
i_wf_addfont(char const *filename) {
  i_clear_error();

  if (!gdi_dll) {
    gdi_dll = GetModuleHandle("GDI32");
    if (gdi_dll) {
      AddFontResourceExAp = (AddFontResourceExA_t)GetProcAddress(gdi_dll, "AddFontResourceExA");
      RemoveFontResourceExAp = (RemoveFontResourceExA_t)GetProcAddress(gdi_dll, "RemoveFontResourceExA");
    }
  }

  if (AddFontResourceExAp && RemoveFontResourceExAp
      && AddFontResourceExAp(filename, FR_PRIVATE, 0)) {
    return 1;
  }
  else if (AddFontResource(filename)) {
    return 1;
  }
  else {
    i_push_errorf(0, "Could not add resource: %ld", GetLastError());
    return 0;
  }
}

/*
=item i_wf_delfont(char const *filename, char const *resource_file)

Deletes a TTF font file as usable by the application.

=cut
 */
int
i_wf_delfont(char const *filename) {
  i_clear_error();

  if (AddFontResourceExAp && RemoveFontResourceExAp
      && RemoveFontResourceExAp(filename, FR_PRIVATE, 0)) {
    return 1;
  }
  else if (RemoveFontResource(filename)) {
    return 1;
  }
  else {
    i_push_errorf(0, "Could not remove resource: %ld", GetLastError());
    return 0;
  }
}

/*
=back

=head1 INTERNAL FUNCTIONS

=over

=item set_logfont(face, size, lf)

Fills in a LOGFONT structure with reasonable defaults.

=cut
*/

static void set_logfont(const char *face, int size, LOGFONT *lf) {
  memset(lf, 0, sizeof(LOGFONT));

  lf->lfHeight = -size; /* character height rather than cell height */
  lf->lfCharSet = DEFAULT_CHARSET;
  lf->lfOutPrecision = OUT_TT_PRECIS;
  lf->lfClipPrecision = CLIP_DEFAULT_PRECIS;
  lf->lfQuality = PROOF_QUALITY;
  strncpy(lf->lfFaceName, face, sizeof(lf->lfFaceName)-1);
  /* NUL terminated by the memset at the top */
}

/*
=item render_text(face, size, text, length, aa, pbm, psz, tm)

renders the text to an in-memory RGB bitmap 

It would be nice to render to greyscale, but Windows doesn't have
native greyscale bitmaps.

=cut
*/
static LPVOID render_text(const char *face, int size, const char *text, int length, int aa,
		   HBITMAP *pbm, SIZE *psz, TEXTMETRIC *tm, int *bbox, int utf8) {
  BITMAPINFO bmi;
  BITMAPINFOHEADER *bmih = &bmi.bmiHeader;
  HDC dc, bmpDc;
  LOGFONT lf;
  HFONT font, oldFont;
  SIZE sz;
  HBITMAP bm, oldBm;
  LPVOID bits;
  int wide_count;
  LPWSTR wide_text;

  dc = GetDC(NULL);
  set_logfont(face, size, &lf);

#ifdef ANTIALIASED_QUALITY
  /* See KB article Q197076
     "INFO: Controlling Anti-aliased Text via the LOGFONT Structure"
  */
  lf.lfQuality = aa ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY;
#endif

  if (utf8) {
    wide_text = utf8_to_wide_string(text, length, &wide_count);
  }
  else {
    wide_text = NULL;
  }

  bmpDc = CreateCompatibleDC(dc);
  if (bmpDc) {
    font = CreateFontIndirect(&lf);
    if (font) {
      oldFont = SelectObject(bmpDc, font);

      GetTextMetrics(bmpDc, tm);
      sz.cx = bbox[BBOX_ADVANCE_WIDTH] - bbox[BBOX_NEG_WIDTH] + bbox[BBOX_POS_WIDTH];
      sz.cy = bbox[BBOX_GLOBAL_ASCENT] - bbox[BBOX_GLOBAL_DESCENT];
      
      memset(&bmi, 0, sizeof(bmi));
      bmih->biSize = sizeof(*bmih);
      bmih->biWidth = sz.cx;
      bmih->biHeight = sz.cy;
      bmih->biPlanes = 1;
      bmih->biBitCount = 24;
      bmih->biCompression = BI_RGB;
      bmih->biSizeImage = 0;
      bmih->biXPelsPerMeter = (LONG)(72 / 2.54 * 100);
      bmih->biYPelsPerMeter = bmih->biXPelsPerMeter;
      bmih->biClrUsed = 0;
      bmih->biClrImportant = 0;
      
      bm = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);

      if (bm) {
	oldBm = SelectObject(bmpDc, bm);
	SetTextColor(bmpDc, RGB(255, 255, 255));
	SetBkColor(bmpDc, RGB(0, 0, 0));
	if (utf8) {
	  TextOutW(bmpDc, -bbox[BBOX_NEG_WIDTH], 0, wide_text, wide_count);
	}
	else {
	  TextOut(bmpDc, -bbox[BBOX_NEG_WIDTH], 0, text, length);
	}
	SelectObject(bmpDc, oldBm);
      }
      else {
	i_push_errorf(0, "Could not create DIB section for render: %ld",
		      GetLastError());
	SelectObject(bmpDc, oldFont);
	DeleteObject(font);
	DeleteDC(bmpDc);
	ReleaseDC(NULL, dc);
	if (wide_text)
	  myfree(wide_text);
	return NULL;
      }
      SelectObject(bmpDc, oldFont);
      DeleteObject(font);
    }
    else {
      if (wide_text)
	myfree(wide_text);
      i_push_errorf(0, "Could not create logical font: %ld",
		    GetLastError());
      DeleteDC(bmpDc);
      ReleaseDC(NULL, dc);
      return NULL;
    }
    DeleteDC(bmpDc);
  }
  else {
    if (wide_text)
      myfree(wide_text);
    i_push_errorf(0, "Could not create rendering DC: %ld", GetLastError());
    ReleaseDC(NULL, dc);
    return NULL;
  }

  if (wide_text)
    myfree(wide_text);

  ReleaseDC(NULL, dc);

  *pbm = bm;
  *psz = sz;

  return bits;
}

/*
=item utf8_to_wide_string(text, text_len, wide_chars)

=cut
*/

static
LPWSTR
utf8_to_wide_string(char const *text, int text_len, int *wide_chars) {
  int wide_count = MultiByteToWideChar(CP_UTF8, 0, text, text_len, NULL, 0);
  LPWSTR result;

  if (wide_count < 0) {
    i_push_errorf(0, "Could not convert utf8: %ld", GetLastError());
    return NULL;
  }
  ++wide_count;
  result = mymalloc(sizeof(WCHAR) * wide_count);
  if (MultiByteToWideChar(CP_UTF8, 0, text, text_len, result, wide_count) < 0) {
    i_push_errorf(0, "Could not convert utf8: %ld", GetLastError());
    return NULL;
  }

  result[wide_count-1] = (WCHAR)'\0';
  *wide_chars = wide_count - 1;

  return result;
}


/*
=back

=head1 BUGS

Should really use a structure so we can set more attributes.

Should support UTF8

=head1 AUTHOR

Tony Cook <tony@develop-help.com>

=head1 SEE ALSO

Imager(3)

=cut
*/
