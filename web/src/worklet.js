// AudioWorklet processor: runs the whole pedalboard on the audio thread.
// The build prepends dsp.js as `const DSP = (...)()`.
//
// Messages in:  {type:"monitor", tuner, analyzer}  (what the page is showing; nothing is sent back for what is off)
//               {type:"params", params} | {type:"source", source} | {type:"pluck", index} | {type:"file", samples, rate} | {type:"playing", playing} | {type:"strum", index}
//               {type:"chord", index} | {type:"chordStop"} | {type:"chordSettings", pattern, bpm}
// Messages out: {tuner, analyzer, inPeak, outPeak, progress} every 1024 frames
//               (raw input for the tuner, post-EQ signal for the spectrum analyser)

const BATCH = 1024;

class PedalboardProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const init = (options && options.processorOptions) || {};

    this.chain = new DSP.FxChain();
    this.chain.prepare(sampleRate, 128);
    this.params = init.params || DSP.defaultParams();
    this.chain.setParameters(this.params);
    this.chain.reset(); // pedals start in their switched state, no fade-in

    this.player = new DSP.TestSignalPlayer();
    this.player.prepare(sampleRate);
    if (init.source) this.player.setSource(init.source);
    if (init.playing === false) this.player.setPlaying(false);
    if (init.chordPattern !== undefined) this.player.setChordPattern(init.chordPattern);
    if (init.chordTempo !== undefined) this.player.setChordTempo(init.chordTempo);

    this.wantTuner = init.tuner !== false;
    this.wantAnalyzer = init.analyzer !== false;
    this.mono = new Float32Array(128);
    this.postEq = new Float32Array(128);
    this.postEqLength = 0;
    this.analyzerTap = (data, n) => {
      if (this.postEqLength + n > this.postEq.length) {
        const grown = new Float32Array((this.postEqLength + n) * 2);
        grown.set(this.postEq.subarray(0, this.postEqLength));
        this.postEq = grown;
      }
      this.postEq.set(data.subarray(0, n), this.postEqLength);
      this.postEqLength += n;
    };
    this.chain.analyzerTap = this.wantAnalyzer ? this.analyzerTap : null;

    this.newBatch();
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  newBatch() {
    this.tuner = this.wantTuner ? new Float32Array(BATCH) : null;
    this.analyzer = this.wantAnalyzer ? new Float32Array(BATCH) : null;
    this.fill = 0;
    this.inPeak = 0;
    this.outPeak = 0;
  }

  onMessage(m) {
    if (m.type === "params") { this.params = m.params; }
    else if (m.type === "monitor") {
      this.wantTuner = m.tuner; this.wantAnalyzer = m.analyzer; // from the next batch on
      this.chain.analyzerTap = this.wantAnalyzer ? this.analyzerTap : null;
    }
    else if (m.type === "source") { this.player.setSource(m.source); }
    else if (m.type === "pluck") { this.player.pluck(m.index); }
    else if (m.type === "strum") { this.player.strum(m.index); }
    else if (m.type === "chord") { this.player.playChord(m.index); }
    else if (m.type === "chordStop") { this.player.stopChord(); }
    else if (m.type === "chordSettings") { this.player.setChordPattern(m.pattern); this.player.setChordTempo(m.bpm); }
    else if (m.type === "file") { this.player.setFile(m.samples, m.rate); }
    else if (m.type === "playing") { this.player.setPlaying(m.playing); }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0], right = out.length > 1 ? out[1] : null;
    const n = left.length;
    if (this.mono.length < n) this.mono = new Float32Array(n);
    const mono = this.mono.subarray(0, n);

    const live = inputs[0] && inputs[0][0];
    if (live && this.player.source === "live") mono.set(live.subarray(0, n)); else mono.fill(0);
    this.player.process(mono, n);

    this.postEqLength = 0;
    this.chain.setParameters(this.params);
    this.chain.process(mono, left, right, n);

    // hand raw input + post-EQ audio to the page in batches
    for (let i = 0; i < n; ) {
      const take = Math.min(n - i, BATCH - this.fill);
      if (this.tuner) this.tuner.set(mono.subarray(i, i + take), this.fill);
      if (this.analyzer && this.postEqLength >= i + take) this.analyzer.set(this.postEq.subarray(i, i + take), this.fill);
      this.fill += take;
      i += take;
      if (this.fill === BATCH) this.flush();
    }
    this.inPeak = Math.max(this.inPeak, this.chain.inputPeak);
    this.outPeak = Math.max(this.outPeak, this.chain.outputPeak);
    return true;
  }

  flush() {
    this.port.postMessage(
      { tuner: this.tuner, analyzer: this.analyzer, inPeak: this.inPeak, outPeak: this.outPeak, progress: this.player.progress,
        chord: this.player.loopingChord() },
      [this.tuner, this.analyzer].filter(Boolean).map((a) => a.buffer)
    );
    this.newBatch();
  }
}

registerProcessor("pedalboard", PedalboardProcessor);
