# Comic Reader

Comics and manuals on an AtGames Legends pinball cabinet (4KP, HDP). Pages on
the playfield, at the screen's full resolution (2160 x 3840 on a 4KP); the
book's cover over a blur of itself on the backglass; the title and page number
on the DMD.

- **Formats:** PDF, CBZ, CBR (RAR), CBT, single pictures (JPG, PNG, WebP, GIF,
  BMP, TIFF), and a folder of pictures as one book (pages in name order). WebP
  pages inside CBZ / CBR work too.
- **Library:** Continue (books you're partway through), Comics, Manuals and
  Settings. A folder of books is a series. Covers are drawn once and cached in
  `data/covers/`. Books with a `ComicInfo.xml` show their series, issue, story,
  date, publisher and credits (press Y), and manga marked right to left opens
  that way. A book without one can be looked up in the Grand Comics Database
  (X on its details card): the match is shown to keep or skip, and kept details
  go in the app's cache, never into the book. comics.org rate-limits its free
  API, so lookups are one at a time and answers are cached for 30 days.
- **Zoom:** Start zooms to 2x, then 3x; the joystick moves around the page. A
  sharp copy of the part on screen is drawn in the background.
- **Page turn:** a page curl (or a slide, or none).
- **Reading:** progress is saved per book, and a book opens where you left it.
  Pages are drawn ahead on the fast cores, so turning is immediate.
- **Settings:** Wi-Fi upload (open the address shown on any phone or computer on
  the same network, enter the PIN, drop files or a folder of pages), Check for
  updates (from this repo's releases; your books and places are kept), Page turn,
  Reading direction (right to left for manga), Forget reading history, Redraw
  covers, About.

## Controls

| | Library | Reading |
|---|---|---|
| Flippers | Previous / next shelf | Previous / next page (hold to flip fast; swapped right to left) |
| Joystick | Move | Left / right turn the page; zoomed in, it moves around |
| A | Open | Next page |
| Start | Open | Zoom: 2x, 3x, back to the whole page |
| Y | The book's details (X there: look it up) | Zoom |
| B | Back, or exit | Whole page (if zoomed), else back to the library |

## On the USB stick

```
external/comic-reader/
  comic-reader.elf, .png, .xml, licenses/
  comics/               books, and series folders of books
  manuals/              the same, for rule sheets and manuals
  data/                 progress.txt, comic-reader.log, covers/
```

## Building

Needs the AtGames External Applications SDK build folder (its Docker image).

```sh
tools/fetch-deps.sh <build folder>          # MuPDF 1.28.5, libarchive 3.8.10, libwebp 1.6.0 sources
rsync -a --exclude .git --exclude build ./ <build folder>/apps/comic-reader/
docker run --rm --platform linux/amd64 -v <build folder>:/workspace \
  atgames-external-sdk:glibc-2.26-sdl2-v1 bash /workspace/apps/comic-reader/tools/build-deps.sh
docker run --rm --platform linux/amd64 -v <build folder>:/workspace \
  atgames-external-sdk:glibc-2.26-sdl2-v1 /workspace/scripts/build-app.sh comic-reader
```

`tools/build-harness.sh` builds an off-cabinet test of the library, covers,
page renderer and upload server (`tools/harness.cpp`) for any aarch64 Linux.

## License

AGPL-3.0 (`LICENSE`), as MuPDF requires. Looked-up details come from the
Grand Comics Database (comics.org), CC BY-SA 4.0. The upload page links to this source.
Third-party notices are in `licenses/` and ship next to the ELF.

Comic Reader ships no comics. The test set used in development is public-domain
Golden Age comics from the Internet Archive (Jumbo Comics, Fiction House, 1946;
My Secret Marriage #8, Superior, 1954).
