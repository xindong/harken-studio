These fixtures contain synthetic, project-generated signals, not recordings.
Run `generate.py` with ffmpeg and its libvorbis encoder to regenerate them.
Each file contains 144137 frames at 48 kHz, encoded at quality 3. Channels
carry distinct tones and short deterministic transients to detect ordering,
seek, loop and tail errors. The temporary packet container is generated in
memory by the tests; only the original Ogg inputs are stored here.
