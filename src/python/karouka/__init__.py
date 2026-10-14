"""karouka: HTML/CSS user interfaces with Python page scripts, rendered by karouka's engine.

    import karouka
    app = karouka.App()
    app.open("ui/index.html", title="Demo", size=(900, 600))
    app.run()          # or: python -m karouka ui/index.html

The engine is the _karouka extension module (src/python/module.cpp); this package is its public
name. App and AppWindow are the app shell (src/python/app.mm); System and View the engine without
windows. AppWindow is a native window: karouka.Window would read as the IDL Window, which page
scripts get as `window` (_karouka.Window). See docs/PYTHON.md.

Installed from the wheel (tools/wheel.py), the package holds Ladybird's resources in res/ and points
the engine at them (KR_RESOURCE_ROOT, unless already set) before any System exists; next to the
build's module it has none, and the engine uses the build's.
"""

import os

_resources = os.path.join(os.path.dirname(os.path.abspath(__file__)), "res")
if os.path.isdir(_resources):
    os.environ.setdefault("KR_RESOURCE_ROOT", _resources)
del _resources

from _karouka import App, AppWindow, DOMException, Error, StaleObjectError, System, View

__version__ = "0.1.0"
__all__ = ["App", "AppWindow", "DOMException", "Error", "StaleObjectError", "System", "View"]
