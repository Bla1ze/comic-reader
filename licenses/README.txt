Comic Reader is free software under the GNU Affero General Public License,
version 3 (Comic-Reader-and-MuPDF-AGPL-3.0.txt). Source code:
https://github.com/Bla1ze/comic-reader

It includes:
  MuPDF 1.28.5, (c) Artifex Software, AGPL-3.0, with a small change to skip
    hidden files in comic archives (tools/patches in the source).
    https://mupdf.com
  libarchive 3.8.10 (RAR support), BSD-style license.
  libwebp 1.6.0's decoder (WebP pages), BSD 3-clause, with Google's patent
    grant (libwebp-COPYING.txt, libwebp-PATENTS.txt). MuPDF is changed to hand
    WebP pictures to it (tools/patches/mupdf-webp.patch in the source).
  From MuPDF's third-party folder: FreeType (FreeType License), libjpeg
    (Independent JPEG Group; "this software is based in part on the work of
    the Independent JPEG Group"), Little CMS (MIT), zlib, jbig2dec (AGPL-3.0),
    OpenJPEG (BSD 2-clause).
  The app's menu art uses the "auto_stories" symbol from Google's Material
    Symbols (Apache-2.0).
  Fonts: URW base-35 (Nimbus) and Noto / SIL fonts bundled in MuPDF, Bangers
    and Comic Neue, all under the SIL Open Font License.
  Looked-up book details come from the Grand Comics Database (comics.org),
    under CC BY-SA 4.0; the app shows the credit with them.
  The controls helper and display profiles are based on the AtGames External
    Applications SDK (https://www.atgames.net/features/external-apps),
    modified, with attribution to AtGames; the panel and drawing code comes
    from Retro Launcher (GPL-3.0, same author), relicensed here under AGPL-3.0.
