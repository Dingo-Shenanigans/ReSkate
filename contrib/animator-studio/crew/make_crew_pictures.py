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
GRIP = HERE.parents[2] / "assets" / "animator_studio" / "market" / "grip.jpg"
OUT = HERE.parents[2] / "assets" / "animator_studio" / "crew"

# team: (portrait, (centre x, centre y, side) of the head-and-shoulders square in it, the shot[, brighten[, blur]])
# brighten: how much brighter a dark picture is made, so it reads small on a dark card (1: as it is).
# blur: for a shot too small to stretch across a card (an avatar of a few hundred pixels), the thumbnail is the shot
# blurred this much and a little darker, a backdrop for the sharp round picture on it (0: as it is).
# The shot GRIP is no picture: the MARKET's griptape (assets/animator_studio/market/grip.jpg), tiled across the card.
CREW = {
    "bonksz": ("bonksz_portrait.png", (435, 450, 800), "bonksz_shot.webp"),
    "fiendskateboards": ("renzo_art.png", (270, 285, 440), "renzo_art.png", 1.7),  # renzoh.
    "reskate_animation_editor": ("phil_portrait.webp", (915, 425, 440), "phil_shot.webp"),  # philsteezy
    "smonky": ("smonk_portrait.png", (412, 175, 210), "smonk_shot.png"),  # smonk
    "myawsometeam": ("setsu_portrait.png", (100, 100, 200), "setsu_shot.webp"),  # Setsu: the avatar whole
    "heatz": ("heatz_portrait.webp", (775, 560, 960), "heatz_shot.webp"),
    "leafbro": ("leaf_portrait.webp", (1005, 930, 2060), "leaf_shot.webp"),  # leaf.bro: the eyes, whole
    # BUILT BY (studio::market::builders), by the name their pictures go under.
    "22sh": ("22sh_portrait.png", (368, 330, 640), "GRIP"),
    "fourth": ("fourth_logo.webp", (570, 430, 740), "fourth_shot.webp"),  # his red tag, round
    "steez": ("steez_portrait.png", (110, 124, 220), "steez_shot.webp"),
    "norms": ("norms_portrait.webp", (560, 470, 270), "norms_portrait.webp"),  # one photo for both
}


# Pictures whose see-through background is white (convert("RGB") turns it black, as renzoh.'s art is meant to be).
ON_WHITE = {"bonksz"}


def opaque(img: Image.Image, white: bool) -> Image.Image:
    """The picture as RGB: a see-through background on white for ON_WHITE, else as convert("RGB") has it (black)."""
    if white and img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        back = Image.new("RGBA", img.size, (255, 255, 255, 255))
        return Image.alpha_composite(back, img).convert("RGB")
    return img.convert("RGB")


def finish(img: Image.Image) -> Image.Image:
    """A touch more contrast and a little sharpening after the scale, so it holds up small on a dark card."""
    img = ImageEnhance.Contrast(img).enhance(1.06)
    img = ImageEnhance.Color(img).enhance(1.04)
    return img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=60, threshold=2))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for team, (portrait, (cx, cy, side), shot, *rest) in CREW.items():
        brighten = rest[0] if rest else 1.0
        blur = rest[1] if len(rest) > 1 else 0
        lift = (lambda img: ImageEnhance.Brightness(img).enhance(brighten)) if brighten != 1.0 else (lambda img: img)
        src = lift(opaque(Image.open(SOURCE / portrait), team in ON_WHITE))
        box = (cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)
        profile = finish(src.crop(box).resize((384, 384), Image.LANCZOS))
        profile.save(OUT / f"{team}_profile.jpg", quality=92, optimize=True, progressive=True)
        if shot == "GRIP":
            # Griptape: the market's tile, a little darker so a card's words stay on top of it.
            tile = Image.open(GRIP).convert("RGB")
            wide = Image.new("RGB", (1280, 800))
            for x in range(0, wide.width, tile.width):
                for y in range(0, wide.height, tile.height):
                    wide.paste(tile, (x, y))
            wide = ImageEnhance.Brightness(wide).enhance(0.8)
        else:
            wide = lift(opaque(Image.open(SOURCE / shot), team in ON_WHITE))
        height = round(wide.height * 960 / wide.width)
        thumbnail = finish(wide.resize((960, height), Image.LANCZOS))
        if blur:
            thumbnail = ImageEnhance.Brightness(thumbnail.filter(ImageFilter.GaussianBlur(blur))).enhance(0.75)
        thumbnail.save(OUT / f"{team}_thumbnail.jpg", quality=90, optimize=True, progressive=True)
        print(team, "profile 384 x 384,", f"thumbnail 960 x {height}")


if __name__ == "__main__":
    main()
