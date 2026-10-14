"""Open an HTML page in a karouka window; its <script type="text/python"> tags run in the page.

    python examples/python_viewer/viewer.py [page.html] [--size WxH] [--frames N --capture out.png --headless]

The engine runs each text/python script in the document's own subinterpreter, with `window`,
`document` and `console`, for pages under the directory the page was loaded from. Needs the
build's karouka/lib on PYTHONPATH, or the karouka wheel installed.
"""

import argparse
import os
import sys

import karouka

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("page", nargs="?", default=os.path.join(HERE, "page.html"))
    parser.add_argument("--size", default="800x600", metavar="WxH")
    parser.add_argument("--headless", action="store_true", help="no window (needs --frames)")
    parser.add_argument("--frames", type=int, metavar="N", help="stop after N frames")
    parser.add_argument("--capture", metavar="PNG", help="write the last frame to a PNG (needs --frames)")
    args = parser.parse_args(argv)
    if (args.headless or args.capture) and not args.frames:
        parser.error("--headless and --capture need --frames")
    width, height = (int(part) for part in args.size.lower().split("x"))

    app = karouka.App(headless=args.headless)
    window = app.open(args.page, size=(width, height))
    frames = 0

    def on_frame(frame_window, now_ms):
        nonlocal frames
        frames += 1
        if args.frames and frames >= args.frames:
            if args.capture:
                window.view.capture_png(args.capture)
            app.stop()

    try:
        app.run(on_frame)
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
