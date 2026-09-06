# CrossInk web-tab patch

This patch adds the "CrossSlate" web tab (tasks / pinned note / doodle canvas
with frames) to a stock CrossInk checkout. It is the exact diff applied on top
of CrossInk main (cab4f24) to build `CrossInk-X4-v14-en.bin`.

## Apply

    git clone https://github.com/uxjulia/crossink
    cd crossink
    git apply crossink-crossslate-webtab.patch
    pio run -e default

## Notes

- Requires the CrossSlate firmware in the other OTA slot to consume the files
  written to the shared SD card (/crossslate/*).
- Endpoints added: GET /crossslate, GET /api/crossslate,
  POST /api/crossslate/save, POST /api/crossslate/doodle,
  POST /api/crossslate/doodle/delete, GET /api/crossslate/debug
