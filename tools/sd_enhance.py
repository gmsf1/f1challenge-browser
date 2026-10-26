#!/usr/bin/env python3
"""Stable Diffusion detail enhancement for SURFACE textures (tarmac, grass, gravel), locally on the Mac GPU.

  work/sd/.venv/bin/python tools/sd_enhance.py IN.png OUT.png --prompt "close-up photo of dry asphalt, fine grain"
        [--scale 2] [--strength 0.35] [--steps 20] [--seed 1] [--pad 64]

SD 1.5 + the ControlNet "tile" model: the texture is enlarged with a plain filter and then re-rendered at low strength, so
the model adds believable fine detail but keeps the layout of the original. NOT for liveries, signs or anything with
lettering: it rewrites what it sees. The texture is wrap-padded before the run and cropped after, so it still tiles.
Setup (once): uv venv work/sd/.venv --python 3.12; uv pip install --python work/sd/.venv/bin/python torch diffusers
transformers accelerate safetensors pillow numpy; the models download on first use (about 2.6 GB).
"""
import argparse, sys, time
import torch
from PIL import Image
from diffusers import ControlNetModel, StableDiffusionControlNetImg2ImgPipeline, UniPCMultistepScheduler

BASE = "stable-diffusion-v1-5/stable-diffusion-v1-5"
TILE = "lllyasviel/control_v11f1e_sd15_tile"
NEG = "blurry, lowres, text, watermark, logo, artifacts, jpeg, painting, cartoon, border, vignette"


def load():
    cn = ControlNetModel.from_pretrained(TILE, torch_dtype=torch.float16, use_safetensors=False)   # the repo only ships a .bin
    pipe = StableDiffusionControlNetImg2ImgPipeline.from_pretrained(BASE, controlnet=cn, torch_dtype=torch.float16, variant="fp16", use_safetensors=True,
                                                                    safety_checker=None, requires_safety_checker=False)
    pipe.scheduler = UniPCMultistepScheduler.from_config(pipe.scheduler.config)
    pipe.to("mps")
    pipe.enable_attention_slicing()
    pipe.vae.enable_tiling()
    return pipe


def wrap_pad(img, p):
    w, h = img.size
    canvas = Image.new(img.mode, (w + 2 * p, h + 2 * p))
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            canvas.paste(img, (p + dx * w, p + dy * h))
    return canvas


def enhance(pipe, img, prompt, scale=2, strength=0.35, steps=20, seed=1, pad=64):
    w, h = img.size
    up = img.convert("RGB").resize((w * scale, h * scale), Image.LANCZOS)
    pad -= pad % 8
    padded = wrap_pad(up, pad)
    pw, ph = padded.size
    padded = padded.resize((pw - pw % 8, ph - ph % 8), Image.LANCZOS) if (pw % 8 or ph % 8) else padded
    out = pipe(prompt=prompt, negative_prompt=NEG, image=padded, control_image=padded, strength=strength,
               num_inference_steps=steps, guidance_scale=6.0, controlnet_conditioning_scale=1.0,
               generator=torch.Generator("cpu").manual_seed(seed)).images[0]
    if out.size != (pw, ph):
        out = out.resize((pw, ph), Image.LANCZOS)
    return out.crop((pad, pad, pad + w * scale, pad + h * scale))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("inp"), ap.add_argument("out")
    ap.add_argument("--prompt", required=True)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--strength", type=float, default=0.35)
    ap.add_argument("--steps", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--pad", type=int, default=64)
    a = ap.parse_args()
    t = time.time()
    pipe = load()
    print(f"loaded in {time.time() - t:.0f}s", flush=True)
    t = time.time()
    res = enhance(pipe, Image.open(a.inp), a.prompt, a.scale, a.strength, a.steps, a.seed, a.pad)
    res.save(a.out)
    print(f"enhanced {a.inp} -> {res.size} in {time.time() - t:.0f}s", flush=True)
