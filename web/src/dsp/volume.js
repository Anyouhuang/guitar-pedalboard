// The HD500X's Volume/Pan models (Source/DSP/fx/Volume.h). Stereo.
import { clamp, f32, Smoothed } from "./core.js";
import { CATEGORY, ENGINE, model, percent } from "./modeltypes.js";

export class VolumeFx {
  prepare(sampleRate, maxBlock) {
    this.variant = this.variant || 0;
    this.gainLeft = new Smoothed(1); this.gainRight = new Smoothed(1);
    this.gainLeft.reset(sampleRate, 0.03); this.gainRight.reset(sampleRate, 0.03);
    this.reset();
  }
  reset() {
    this.gainLeft.setCurrentAndTarget(this.gainLeft.target);
    this.gainRight.setCurrentAndTarget(this.gainRight.target);
  }
  setModel(variant) { this.variant = clamp(variant | 0, 0, 1); }
  setParameters(k) {
    const position = f32(k[0] / 100);
    if (this.variant === 0) {
      this.gainLeft.setTarget(f32(position * position));
      this.gainRight.setTarget(f32(position * position));
    } else {
      this.gainLeft.setTarget(f32(Math.min(1, 2 - 2 * position)));
      this.gainRight.setTarget(f32(Math.min(1, 2 * position)));
    }
  }
  process(left, right, n) {
    const gl = this.gainLeft, gr = this.gainRight;
    for (let i = 0; i < n; ++i) { left[i] *= gl.next(); right[i] *= gr.next(); }
  }
}

/** In the order of the variants. */
export const VOLUME_MODELS = [
  model("volume_pedal", "Volume Pedal", CATEGORY.volume, ENGINE.volumeFx, 0, "Volume pedal (100 % = unity)", [percent("Volume", 100)]),
  model("pan", "Pan", CATEGORY.volume, ENGINE.volumeFx, 1, "Pan / balance (50 % = centre)", [percent("Pan", 50)]),
];
