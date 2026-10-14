"""python -m karouka page.html: open a page in a window and run it until the window closes."""

import argparse
import sys

from . import App


def parse_size(text):
    try:
        width, height = (int(part) for part in text.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected WxH, not {text!r}") from None
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError(f"expected a positive WxH, not {text!r}")
    return width, height


def main(argv=None):
    parser = argparse.ArgumentParser(prog="python -m karouka", description=__doc__)
    parser.add_argument("page", help="the HTML file to open; its directory is the app root for text/python")
    parser.add_argument("--title", help="the window's title while the document has none (default: the file's name)")
    parser.add_argument("--size", type=parse_size, default=(1024, 768), metavar="WxH", help="content size in points")
    parser.add_argument("--software", action="store_true", help="rasterize on the CPU (the software backend)")
    parser.add_argument("--headless", action="store_true", help="no window: step frames back to back (needs --frames)")
    parser.add_argument("--frames", type=int, metavar="N", help="return after N frames")
    parser.add_argument("--capture", metavar="PNG", help="write the last frame to a PNG (needs --frames)")
    parser.add_argument("--verbose", action="store_true", help="the engine's verbose logging")
    args = parser.parse_args(argv)
    if (args.headless or args.capture) and not args.frames:
        parser.error("--headless and --capture need --frames")

    app = App(backend="software" if args.software else "metal", headless=args.headless, verbose=args.verbose)
    window = app.open(args.page, title=args.title, size=args.size)
    on_frame = None
    if args.frames:
        frames = 0

        def on_frame(frame_window, now_ms):
            nonlocal frames
            if frame_window is not window:
                return
            frames += 1
            if frames >= args.frames:
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
