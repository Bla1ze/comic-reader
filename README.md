# Comic Reader

Comics and manuals on an AtGames Legends pinball cabinet (4KP, HDP). Pages on
the playfield, at the screen's full resolution (2160 x 3840 on a 4KP); the
book's cover, title and progress on the backglass; the page number on the DMD.

- **Formats:** PDF, CBZ, CBR (RAR), CBT, single pictures (JPG, PNG, GIF,
  BMP, TIFF), and a folder of pictures as one book (pages in name order).
- **Library:** Continue (books you're partway through), Comics, Manuals and
  Wi-Fi upload. A folder of books is a series. Covers are drawn once and
  cached in `data/covers/`.
- **Reading:** progress is saved per book, and a book opens where you left it.
  Pages are drawn ahead on the fast cores, so turning is immediate.
- **Wi-Fi upload:** open the tab, then the address shown, on any phone or
  computer on the same network, and enter the PIN. Pick Comics or Manuals and a
  series (or a new one), and drop files or a folder of pages.

## Controls

| | Library | Reading |
|---|---|---|
| Flippers | Previous / next shelf | Previous / next page (hold to flip fast) |
| Joystick | Move | Left / right: turn the page |
| A or Start | Open | A: next page; Start: show the page number |
| B | Back, or exit | Back to the library |

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
tools/fetch-deps.sh <build folder>          # MuPDF 1.28.5 + libarchive 3.8.10 sources
rsync -a --exclude .git --exclude build ./ <build folder>/apps/comic-reader/
docker run --rm --platform linux/amd64 -v <build folder>:/workspace \
  atgames-external-sdk:glibc-2.26-sdl2-v1 bash /workspace/apps/comic-reader/tools/build-deps.sh
docker run --rm --platform linux/amd64 -v <build folder>:/workspace \
  atgames-external-sdk:glibc-2.26-sdl2-v1 /workspace/scripts/build-app.sh comic-reader
```

`tools/build-harness.sh` builds an off-cabinet test of the library, covers,
page renderer and upload server (`tools/harness.cpp`) for any aarch64 Linux.

## License

AGPL-3.0 (`LICENSE`), as MuPDF requires. The upload page links to this source.
Third-party notices are in `licenses/` and ship next to the ELF.

Comic Reader ships no comics. The test set used in development is public-domain
Golden Age comics from the Internet Archive (Jumbo Comics, Fiction House, 1946;
My Secret Marriage #8, Superior, 1954).
