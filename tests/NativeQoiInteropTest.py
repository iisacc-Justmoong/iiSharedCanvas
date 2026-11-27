"""Compare the native QOI codec with an independently selected FFmpeg binary."""
import subprocess
import sys

driver, ffmpeg = sys.argv[1:]
def supports(flag, name):
    listing = subprocess.run([ffmpeg, "-hide_banner", flag], check=True, capture_output=True, text=True)
    return any(len(fields) >= 2 and fields[1] == name
               for fields in (line.split() for line in listing.stdout.splitlines()))

if not (supports("-encoders", "qoi") and supports("-decoders", "qoi")
        and supports("-demuxers", "qoi_pipe")):
    print("SKIP: the selected FFmpeg does not supply QOI encoding and decoding")
    sys.exit(77)

expected = bytes([17,34,51,0, 17,34,51,0, 17,34,51,128, 17,34,51,255,
                  17,34,51,255, 255,255,255,255, 1,2,3,255, 0,0,0,0])
native = subprocess.run([driver, "--encode-hex"], check=True, capture_output=True)
encoded = bytes.fromhex(native.stdout.decode("ascii"))
decoded = subprocess.run([ffmpeg, "-v", "error", "-f", "qoi_pipe", "-i", "pipe:0",
                          "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"],
                         input=encoded, check=True, capture_output=True)
assert decoded.stdout == expected, "FFmpeg decoded different RGBA pixels"
external = subprocess.run([ffmpeg, "-v", "error", "-f", "rawvideo", "-pixel_format", "rgba",
                           "-video_size", "4x2", "-i", "pipe:0", "-frames:v", "1",
                           "-c:v", "qoi", "-f", "image2pipe", "pipe:1"],
                          input=expected, check=True, capture_output=True)
subprocess.run([driver, "--decode-hex"], input=external.stdout.hex().encode("ascii"),
               check=True, capture_output=True)
