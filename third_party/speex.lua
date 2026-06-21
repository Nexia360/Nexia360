group("third_party")
project("speex")
  uuid("3d6f1c0e-7a4b-4f2a-9c1d-2b5e8a9f4c10")
  kind("StaticLib")
  language("C")
  defines({
    "HAVE_CONFIG_H",
  })
  includedirs({
    "speex/win32",     -- config.h (FLOATING_POINT, USE_SMALLFT)
    "speex/include",   -- public <speex/*.h>
    "speex/libspeex",  -- internal headers
  })
  filter("action:vs*")
    -- Speex is C89-ish; quiet the expected MSVC warnings.
    disablewarnings({ "4244", "4267", "4305", "4996" })
  filter({})
  files({
    "speex/libspeex/bits.c",
    "speex/libspeex/cb_search.c",
    "speex/libspeex/exc_5_64_table.c",
    "speex/libspeex/exc_5_256_table.c",
    "speex/libspeex/exc_8_128_table.c",
    "speex/libspeex/exc_10_16_table.c",
    "speex/libspeex/exc_10_32_table.c",
    "speex/libspeex/exc_20_32_table.c",
    "speex/libspeex/filters.c",
    "speex/libspeex/gain_table.c",
    "speex/libspeex/gain_table_lbr.c",
    "speex/libspeex/hexc_table.c",
    "speex/libspeex/hexc_10_32_table.c",
    "speex/libspeex/high_lsp_tables.c",
    "speex/libspeex/lpc.c",
    "speex/libspeex/lsp.c",
    "speex/libspeex/lsp_tables_nb.c",
    "speex/libspeex/ltp.c",
    "speex/libspeex/modes.c",
    "speex/libspeex/modes_wb.c",
    "speex/libspeex/nb_celp.c",
    "speex/libspeex/quant_lsp.c",
    "speex/libspeex/sb_celp.c",
    "speex/libspeex/smallft.c",
    "speex/libspeex/speex.c",
    "speex/libspeex/speex_callbacks.c",
    "speex/libspeex/speex_header.c",
    "speex/libspeex/stereo.c",
    "speex/libspeex/vbr.c",
    "speex/libspeex/vq.c",
    "speex/libspeex/window.c",
  })
