"""Claude Dash desk cases for the Waveshare ESP32-S3-Touch-LCD-2.1.

Parametric build123d model. Everything prints without supports on a
Bambu Lab A1 mini (180 x 180 x 180 mm):

  puck        holds the display; print BACK face down
  ring        black bezel that press-fits over the glass edge; print FRONT face down
  halo        round base + slim fin leaning back 15 deg; print base down
  ribbon      one bent band (Studio Display-style); print on its SIDE
  fit_test    first 5.6 mm of the puck: check the glass fit + ring before the full print

The puck attaches to either stand with 3 magnets (6 x 2 mm) or a drop of glue.

Board numbers come from Waveshare's dimension drawing (glass 75.00, active area
53.28, board 66.00 flat-to-flat). The drawing has no thicknesses: MODULE_DEPTH
and PORT_Z are estimates. Measure your board and set them before printing.

Run:  python claude_dash_case.py          (writes stl/, step/, 3mf/ next to this file)
"""
from math import cos, radians, sin
from pathlib import Path

from build123d import (Align, Axis, Box, Cylinder, Location, Mesher, Plane, Pos, Rot,
                       Vector, chamfer, export_step, export_stl, extrude, fillet,
                       offset, FilletPolyline, Kind, Unit, make_face)

# --------------------------------------------------------------------- the board
GLASS_D = 75.00          # cover glass diameter (drawing)
ACTIVE_D = 53.28         # visible display diameter (drawing)
MODULE_DEPTH = 11.0      # glass front -> tallest part on the back. MEASURE THIS.
PORT_Z = 7.0             # glass front -> centre of the USB-C ports (they sit on the back)
BTN_Z = 6.2              # glass front -> centre of BOOT/RESET/switch on the board edge
# Front view (looking at the screen), origin = screen centre, y up.
PORTS = [((23.9, -19.4), -45.0),     # "USB" port, bottom right, points down-right
         ((-23.9, -19.4), -135.0)]   # "UART" port, bottom left, points down-left
BTNS_Y = {"boot": 8.8, "reset": 1.2}  # pinholes on the right edge
SWITCH_Y = -8.7                       # battery ON/OFF switch, right edge

# --------------------------------------------------------------------- the case
FIT = 0.30               # radial clearance around the glass
R = 43.0                 # puck radius (diameter 86)
RING_OD, RING_ID, RING_T = 82.0, 72.6, 1.6   # bezel covers 1.2 mm of the glass rim
RING_FIT = 0.075         # radial clearance for the press-fit ring
FLOOR = 5.0              # material behind the board (room for magnets)
D = RING_T + MODULE_DEPTH + FLOOR            # puck depth (17.6)
PLUG_W, PLUG_H = 13.5, 8.0                   # slot for a straight USB-C plug
FIN_W, FIN_T = 30.0, 5.0                     # stand blade
SLOT_DEPTH = 1.5                             # locating slot in the puck back
MAGNET_D, MAGNET_H = 6.2, 2.3                # 6 x 2 mm magnets, pocket a bit larger
MAGNETS_Y = (10.0, -14.0)                    # along the puck's vertical centreline
TILT = 15.0              # display leans back this much
PUCK_FRONT = (0.0, -6.0, 76.0)               # world position of the screen centre (x, y back, z up)

OUT = Path(__file__).resolve().parent


def puck():
    body = Cylinder(R, D, align=(Align.CENTER, Align.CENTER, Align.MAX))   # front face at z=0
    body = fillet(body.edges().group_by(Axis.Z)[-1], radius=1.6)          # soft front edge
    body = chamfer(body.edges().group_by(Axis.Z)[0], length=1.2)          # bed side: chamfer, not fillet
    # bezel recess and board pocket
    body -= Cylinder(RING_OD / 2 + RING_FIT, RING_T, align=(Align.CENTER, Align.CENTER, Align.MAX))
    body -= Pos(0, 0, -RING_T) * Cylinder(GLASS_D / 2 + FIT, MODULE_DEPTH,
                                          align=(Align.CENTER, Align.CENTER, Align.MAX))
    # slots for the two USB-C plugs, along each port's axis, through the wall
    zp = -(RING_T + PORT_Z)
    for (px, py), ang in PORTS:
        d = Vector(cos(radians(ang)), sin(radians(ang)), 0)
        c = Vector(px, py, zp) + d * 14
        body -= Location(c, (0, 0, ang)) * Box(34, PLUG_W, PLUG_H)
    # service holes on the right edge: BOOT, RESET pinholes and the battery switch
    zb = -(RING_T + BTN_Z)
    for y in BTNS_Y.values():
        body -= Location((R - 4, y, zb), (0, 90, 0)) * Cylinder(1.0, 14)
    body -= Pos(R - 4, SWITCH_Y, zb) * Box(14, 5.5, 3.2)
    # back: locating slot for the stand blade (runs from above centre to the bottom edge)
    slot_top, slot_bot = 20.0, -(R + 2)
    body -= Pos(0, (slot_top + slot_bot) / 2, -D) * Box(
        FIN_W + 0.4, slot_top - slot_bot, SLOT_DEPTH * 2)
    for y in MAGNETS_Y:
        body -= Pos(0, y, -D + SLOT_DEPTH) * Cylinder(
            MAGNET_D / 2, MAGNET_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return body


def ring():
    r = Cylinder(RING_OD / 2, RING_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
    r -= Cylinder(RING_ID / 2, RING_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
    inner_top = [e for e in r.edges().group_by(Axis.Z)[-1] if e.radius < RING_OD / 2 - 1]
    return chamfer(inner_top, length=0.6)     # front face is z=RING_T: print it face down


def fit_test():
    """Front 5.6 mm of the puck: glass pocket + bezel recess, 15-20 min to print."""
    p = puck()
    keep = Cylinder(R + 1, RING_T + 4, align=(Align.CENTER, Align.CENTER, Align.MAX))
    return p & keep


# ---------------------------------------------------------------- placing the puck
def _frames():
    t = radians(TILT)
    n = Vector(0, -cos(t), sin(t))      # screen normal (towards the viewer, tilted up)
    u = Vector(0, sin(t), cos(t))       # "up" along the screen
    front = Vector(*PUCK_FRONT)
    back = front - n * D                # centre of the puck's back face
    return n, u, front, back


def puck_location():
    _, _, front, _ = _frames()
    return Location(front, (90 - TILT, 0, 0))      # local z -> n, local y -> u


def blade_location():
    """Frame on the puck's back plane: x across, y into the stand, z up along the puck."""
    _, _, _, back = _frames()
    return Location(back, (-TILT, 0, 0))


def _magnet_pockets():
    cut = None
    for y in MAGNETS_Y:
        c = Location((0, -SLOT_DEPTH, y), (-90, 0, 0)) * Cylinder(
            MAGNET_D / 2, MAGNET_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
        cut = c if cut is None else cut + c
    return blade_location() * cut


def halo():
    """Round base + one slim fin leaning back. Print with the base on the bed."""
    _, u, _, back = _frames()
    t_bottom = (back.Z - 3.0) / cos(radians(TILT))            # fin reaches into the base
    blade = Box(FIN_W, FIN_T + SLOT_DEPTH, 20 + t_bottom)
    blade = fillet(blade.edges().filter_by(Axis.Z), radius=2.6)           # soft long edges
    blade = fillet(blade.edges().group_by(Axis.Z)[-1], radius=2.0)        # rounded top
    fin = blade_location() * Pos(0, (FIN_T - SLOT_DEPTH) / 2, (20 - t_bottom) / 2) * blade
    fin_foot_y = (back - u * ((back.Z - 9) / cos(radians(TILT)))).Y
    base = Pos(0, fin_foot_y - 4, 0) * Cylinder(42, 9, align=(Align.CENTER, Align.CENTER, Align.MIN))
    base = fillet(base.edges().group_by(Axis.Z)[-1], radius=3.5)
    base = chamfer(base.edges().group_by(Axis.Z)[0], length=0.6)
    stand = base + fin
    stand -= _magnet_pockets()
    stand -= Pos(0, 0, -50) * Box(200, 200, 100)              # nothing below the table
    # cable notch at the back of the base
    stand -= Pos(0, fin_foot_y + 38, 9) * Box(8, 20, 8)
    return stand


def ribbon():
    """One band: foot on the desk, a big bend, then up the puck's back. Print on its side."""
    _, u, _, back = _frames()
    t = radians(TILT)
    centre_off = (FIN_T - SLOT_DEPTH) / 2                     # band centre behind the puck back
    nb = Vector(0, cos(t), -sin(t))                           # into the stand
    top = back + nb * centre_off + u * 20
    pad_bottom = back + nb * centre_off - u * 24           # flat against the puck's back (magnets)
    lean = radians(32)                                     # then a long straight leg down and back
    w = Vector(0, sin(lean), -cos(lean))
    corner = pad_bottom + w * ((pad_bottom.Z - FIN_T / 2) / cos(lean))
    # the foot runs forward to just in front of the screen: the bend behind and the foot in
    # front keep it from tipping when you tap the screen
    front_end = Vector(0, PUCK_FRONT[1] - 14, FIN_T / 2)
    path = FilletPolyline((front_end.Y, front_end.Z), (corner.Y, corner.Z),
                          (pad_bottom.Y, pad_bottom.Z), (top.Y, top.Z), radius=12)
    profile = make_face(offset(path, amount=FIN_T / 2, kind=Kind.ARC))   # band cross-section
    band = Plane.YZ * profile
    stand = extrude(band, amount=FIN_W / 2, both=True)
    stand -= _magnet_pockets()
    return stand


# --------------------------------------------------------------------- export
def export_all():
    parts = {
        "puck": puck(),
        "ring": ring(),
        "halo_stand": halo(),
        "ribbon_stand": ribbon(),
        "fit_test": fit_test(),
    }
    for d in ("stl", "step", "3mf"):
        (OUT / d).mkdir(exist_ok=True)
    # print orientation: puck back down (as modelled), ring front down, ribbon on its side
    print_pose = {
        "puck": Rot(0, 0, 0),
        "ring": Rot(180, 0, 0),
        "halo_stand": Rot(0, 0, 0),
        "ribbon_stand": Rot(0, 90, 0),
        "fit_test": Rot(0, 0, 0),
    }
    for name, shape in parts.items():
        posed = print_pose[name] * shape
        bb = posed.bounding_box()
        posed = Pos(-bb.center().X, -bb.center().Y, -bb.min.Z) * posed   # on the bed, centred
        export_stl(posed, OUT / "stl" / f"{name}.stl", tolerance=0.01, angular_tolerance=0.1)
        export_step(shape, OUT / "step" / f"{name}.step")
        size = posed.bounding_box().size
        print(f"{name:13s} {size.X:6.1f} x {size.Y:6.1f} x {size.Z:6.1f} mm  "
              f"volume {shape.volume / 1000:6.1f} cm3")
    # one 3MF per concept (parts in print orientation; arrange/split plates in Bambu Studio)
    for concept, names in {"halo": ["puck", "ring", "halo_stand"],
                           "ribbon": ["puck", "ring", "ribbon_stand"]}.items():
        m = Mesher(unit=Unit.MM)
        x = 0.0
        for name in names:
            posed = print_pose[name] * parts[name]
            bb = posed.bounding_box()
            posed = Pos(x - bb.min.X, -bb.center().Y, -bb.min.Z) * posed
            x += bb.size.X + 8
            m.add_shape(posed, part_number=name)
        m.write(OUT / "3mf" / f"claude_dash_{concept}.3mf")
    return parts


def assembly(concept):
    """Parts placed as they stand on the desk (for renders)."""
    p = puck_location() * puck()
    r = puck_location() * Pos(0, 0, -RING_T) * ring()
    s = halo() if concept == "halo" else ribbon()
    return {"puck": p, "ring": r, "stand": s}


if __name__ == "__main__":
    export_all()


def export_render_meshes(folder):
    """Assemblies as they stand on the desk, one STL per part (used by render/view.html)."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    for concept in ("halo", "ribbon"):
        for name, shape in assembly(concept).items():
            export_stl(shape, folder / f"{concept}_{name}.stl", tolerance=0.02, angular_tolerance=0.15)
    # where the screen sits, for the renderer
    loc = puck_location()
    o = loc.position
    _, u, _, _ = _frames()
    t = radians(TILT)
    import json
    (folder / "screen.json").write_text(json.dumps({
        "center": [o.X, o.Y, o.Z], "normal": [0, -cos(t), sin(t)], "up": [u.X, u.Y, u.Z],
        "glass_r": GLASS_D / 2, "active_r": ACTIVE_D / 2, "recess": RING_T}))
