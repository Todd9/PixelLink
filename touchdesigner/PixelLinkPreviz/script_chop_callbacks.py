# script_chop_callbacks — paste into the Callbacks DAT of a Script CHOP
#
# Outputs one sample per LED, ready for Geometry COMP instancing:
#   tx ty tz   position in meters, TouchDesigner axes (Y up)
#   r g b      current color, 0-1
#   scale      instance size per feature (the feature column of layout.csv)
#
# Reads the layout from the Table DAT named 'layout' (layout.csv).

import numpy as np

core = op('pl_core').module

INCH = 0.0254   # layout is in inches; TouchDesigner scene in meters
# Instance size per feature name in layout.csv, in meters; others get DEFAULT_SIZE.
SIZES = {'strip': 0.012}
DEFAULT_SIZE = 0.05
GAIN = 1.0      # brighten the preview if it looks dim (colors are clipped at 1)


def onSetupParameters(scriptOp):
    return


def onPulse(par):
    return


def onGetCookLevel(scriptOp):
    return CookLevel.ALWAYS   # cook every frame


def onCook(scriptOp):
    lay = core.layout_from_table(op('layout'))
    n = len(lay)
    colors = np.clip(lay.colors() * GAIN, 0.0, 1.0)
    scale = np.array([SIZES.get(f, DEFAULT_SIZE) for f in lay.feature], np.float32)

    scriptOp.clear()
    scriptOp.numSamples = n
    # Layout Z-up -> TouchDesigner Y-up: (x, y, z) -> (x, z, -y)
    for name, vals in (('tx', lay.x * INCH), ('ty', lay.z * INCH), ('tz', -lay.y * INCH),
                       ('r', colors[:, 0]), ('g', colors[:, 1]), ('b', colors[:, 2]),
                       ('scale', scale)):
        scriptOp.appendChan(name).vals = vals.tolist()
    return
