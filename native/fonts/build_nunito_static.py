"""Rebuild the embedded, static ExtraBold face (maintainer-only, not a runtime dependency).

    python -m pip install fonttools==4.68.0
    python fonts/build_nunito_static.py

The variable source and its OFL license remain alongside the generated face.
"""
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parent
font = TTFont(ROOT / "NunitoSans-Variable.ttf", recalcTimestamp=False)
instantiateVariableFont(
    font,
    {"wght": 800, "wdth": 100, "opsz": 12, "YTLC": 500},
    inplace=True,
)
# A fully static face must not retain variable-axis STAT naming. DirectWrite
# otherwise derives a different WWS family from the non-elidable YTLC axis.
if "STAT" in font:
    del font["STAT"]
font["name"].removeNames(nameID=25)  # Variable PostScript-name prefix.
# Match the proven static Inter ExtraBold naming model: the legacy four-style
# family is separate; typographic/WWS names group by Nunito Sans + weight 800.
names = {
    1: "Nunito Sans ExtraBold",
    2: "Regular",
    3: "Watchman;NunitoSans-ExtraBold;static-800-100-12-500",
    4: "Nunito Sans ExtraBold",
    6: "NunitoSans-ExtraBold",
    16: "Nunito Sans",
    17: "ExtraBold",
    21: "Nunito Sans",
    22: "ExtraBold",
}
for name_id, value in names.items():
    font["name"].removeNames(nameID=name_id)
    font["name"].setName(value, name_id, 3, 1, 0x409)
font["OS/2"].usWeightClass = 800
# Regular within its legacy ExtraBold family; actual weight remains OS/2 800.
font["OS/2"].fsSelection = (font["OS/2"].fsSelection | (1 << 6)) & ~((1 << 0) | (1 << 5))
font["head"].macStyle &= ~3
font.save(ROOT / "NunitoSans-ExtraBold.ttf")
