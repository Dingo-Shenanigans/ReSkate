"""THE CREW's pictures for the Animator Studio, built into ReSkate.dll (cmake/Runtime.cmake, studio_market_art.cpp):

    assets/animator_studio/crew/<team>_profile.jpg    384 x 384, head and shoulders: drawn round on ABOUT and the MARKET
    assets/animator_studio/crew/<team>_thumbnail.jpg  960 wide, the whole shot: across the top of their crew cards

<team> is their Thunderstore team, lower case (studio::market::crew_members), or a builder's picture name (builders). Each member's sources are in source/, and
where the face sits in the portrait is set below. Run with Python and Pillow:

    python make_crew_pictures.py
"""
from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "source"
OUT = HERE.parents[2] / "assets" / "animator_studio" / "crew"

# team: (portrait, (centre x, centre y, side) of the head-and-shoulders square in it, the shot)
CREW = {
    "bonksz": ("bonksz_portrait.webp", (312, 336, 600), "bonksz_shot.webp"),
    # BUILT BY (studio::market::builders), by the name their pictures go under.
    "fourth": ("fourth_portrait.webp", (995, 520, 940), "fourth_shot.webp"),
}


def finish(img: Image.Image) -> Image.Image:
    """A touch more contrast and a little sharpening after the scale, so it holds up small on a dark card."""
    img = ImageEnhance.Contrast(img).enhance(1.06)
    img = ImageEnhance.Color(img).enhance(1.04)
    return img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for team, (portrait, (cx, cy, side), shot) in CREW.items():
        src = Image.open(SOURCE / portrait).convert("RGB")
        box = (cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)
        profile = finish(src.crop(box).resize((384, 384), Image.LANCZOS))
        profile.save(OUT / f"{team}_profile.jpg", quality=92, optimize=True, progressive=True)
        wide = Image.open(SOURCE / shot).convert("RGB")
        height = round(wide.height * 960 / wide.width)
        thumbnail = finish(wide.resize((960, height), Image.LANCZOS))
        thumbnail.save(OUT / f"{team}_thumbnail.jpg", quality=90, optimize=True, progressive=True)
        print(team, "profile 384 x 384,", f"thumbnail 960 x {height}")


if __name__ == "__main__":
    main()
