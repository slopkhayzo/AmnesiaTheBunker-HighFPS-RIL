"""Summarise frame traces written by [Telemetry] FrameTrace.

Usage:
  python tools/analyze_frames.py [trace.frames.bin]
      Frame-time percentiles and lows, where the time of a frame goes (split
      by whether the frame ran a simulation tick), the pacing pattern between
      ticks, and how evenly the view turns per frame during fast turns.

  python tools/analyze_frames.py --compare baseline.bin play.bin
      What a tick frame and a plain frame cost in each trace, side by side.
      With a baseline run (tools\\run_baseline.cmd) against a play run, the
      difference is what the plugin's layers add.

Without a path the newest trace beside the plugin is used.
"""
import math
import struct
import sys
from pathlib import Path

HEADER = struct.Struct("<8sqII")
STAMPS = ["fixedBegin", "fixedEnd", "variableEnd", "renderBegin",
          "paceWaitEnd", "renderOriginal", "renderEnd", "swapEnter", "waitEnd"]
RECORD = struct.Struct("<q9I5fIHH")
BIN = Path(__file__).resolve().parent.parent / "build-x64" / "bin"


def percentile(ordered, fraction):
    if not ordered:
        return float("nan")
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def describe(values):
    ordered = sorted(values)
    if not ordered:
        return "n=0"
    return (f"n={len(ordered):>7}  mean={sum(ordered) / len(ordered):6.3f}  "
            f"p50={percentile(ordered, 0.50):6.3f}  "
            f"p95={percentile(ordered, 0.95):6.3f}  "
            f"p99={percentile(ordered, 0.99):6.3f}  max={ordered[-1]:7.3f}")


def wrap(angle):
    return (angle + math.pi) % (2.0 * math.pi) - math.pi


def load(path):
    """Gameplay frames of a trace, each with its duration in ms."""
    data = path.read_bytes()
    magic, frequency, record_size, stamp_count = HEADER.unpack_from(data, 0)
    if magic != b"ATBFRM02" or record_size != RECORD.size or \
            stamp_count != len(STAMPS):
        sys.exit(f"{path}: not a frame trace this script understands "
                 f"(written by another plugin build?)")
    to_ms = 1000.0 / frequency
    frames = []
    for offset in range(HEADER.size, len(data) - RECORD.size + 1, RECORD.size):
        fields = RECORD.unpack_from(data, offset)
        frames.append({
            "presented": fields[0],
            "stamps": dict(zip(STAMPS, (v * to_ms for v in fields[1:10]))),
            "yaw": fields[10], "pitch": fields[11],
            "leadYaw": fields[12], "leadPitch": fields[13],
            "alpha": fields[14], "serial": fields[15],
            "ticks": fields[16], "flags": fields[17] & 0xFF,
            "framesPerTick": fields[17] >> 8,
        })
    for previous, frame in zip(frames, frames[1:]):
        frame["ms"] = (frame["presented"] - previous["presented"]) * to_ms
    frames = frames[1:]
    # Gameplay only: the scene has a listener camera, and no loading-sized
    # stalls or catch-up bursts.
    play = [f for f in frames
            if f["flags"] & 5 and f["ms"] < 100.0 and f["ticks"] <= 1]
    return frames, play


def span(group, begin, end):
    """Durations between two stamps, for frames that reached both."""
    if begin == "start":
        return [f["stamps"][end] for f in group if f["stamps"][end] > 0]
    return [f["stamps"][end] - f["stamps"][begin] for f in group
            if 0 < f["stamps"][begin] <= f["stamps"][end]]


def pace_wait(frame):
    stamps = frame["stamps"]
    if stamps["paceWaitEnd"] > 0 and stamps["renderBegin"] > 0:
        return stamps["paceWaitEnd"] - stamps["renderBegin"]
    return 0.0


def busy(group):
    """Frame start to swap entry without the pacer's wait: the frame's work."""
    return [f["stamps"]["swapEnter"] - pace_wait(f) for f in group
            if f["stamps"]["swapEnter"] > 0]


def median(values):
    return percentile(sorted(values), 0.5)


def compare(first_path, second_path):
    rows = [
        ("tick frame: fixed update", lambda t, p: span(t, "fixedBegin", "fixedEnd")),
        ("tick frame: variable update", lambda t, p: span(t, "fixedEnd", "variableEnd")),
        ("tick frame: update end to swap", lambda t, p: [
            f["stamps"]["swapEnter"] - f["stamps"]["variableEnd"] - pace_wait(f)
            for f in t if f["stamps"]["variableEnd"] > 0
            and f["stamps"]["swapEnter"] > 0]),
        ("tick frame: all work", lambda t, p: busy(t)),
        ("plain frame: all work", lambda t, p: busy(p)),
    ]
    traces = []
    for path in (first_path, second_path):
        frames, play = load(path)
        if len(play) < 500:
            sys.exit(f"{path.name}: only {len(play)} gameplay frames")
        traces.append(([f for f in play if f["ticks"] == 1],
                       [f for f in play if f["ticks"] == 0]))
        print(f"{path.name}: {len(play)} gameplay frames, "
              f"{len(traces[-1][0])} ticks")
    print(f"\nWork per frame in ms, excluding waits "
          f"(A = {first_path.name}, B = {second_path.name})")
    print(f"{'':<32}{'A p50':>8}{'B p50':>8}{'B - A':>8}   "
          f"{'A p95':>8}{'B p95':>8}{'B - A':>8}")
    for label, extract in rows:
        a = sorted(extract(*traces[0]))
        b = sorted(extract(*traces[1]))
        a50, b50 = percentile(a, 0.5), percentile(b, 0.5)
        a95, b95 = percentile(a, 0.95), percentile(b, 0.95)
        print(f"{label:<32}{a50:8.3f}{b50:8.3f}{b50 - a50:+8.3f}   "
              f"{a95:8.3f}{b95:8.3f}{b95 - a95:+8.3f}")
    print("\nOnly meaningful if both runs stood in the same place doing the "
          "same thing.")


def report(path):
    frames, play = load(path)
    print(f"{path.name}: {len(frames)} frames, {len(play)} in gameplay")
    if len(play) < 1000:
        sys.exit("not enough gameplay frames to analyse")

    times = sorted(f["ms"] for f in play)
    print("\n== Frame time (ms), gameplay ==")
    print("all frames   ", describe(times))
    print(f"average FPS  {1000.0 * len(times) / sum(times):7.1f}")
    for label, fraction in (("1% low", 0.01), ("0.1% low", 0.001)):
        worst = times[-max(1, int(len(times) * fraction)):]
        print(f"{label:<9} FPS {1000.0 * len(worst) / sum(worst):7.1f}   "
              f"(mean of the slowest {fraction:.1%} of frames)")
    for label, fraction in (("p99", 0.99), ("p99.9", 0.999)):
        print(f"{label:<9} frame time {percentile(times, fraction):6.3f} ms "
              f"= {1000.0 / percentile(times, fraction):6.1f} FPS")
    middle = median(times)
    for tolerance in (0.05, 0.10, 0.25):
        inside = sum(1 for t in times if abs(t - middle) <= tolerance * middle)
        print(f"within {tolerance:.0%} of the median frame time: "
              f"{inside / len(times):6.1%}")

    paced = [f for f in play if f["framesPerTick"]]
    if paced:
        print("\n== Even pacing ==")
        counts = {}
        for f in paced:
            counts[f["framesPerTick"]] = counts.get(f["framesPerTick"], 0) + 1
        print(f"paced frames: {len(paced) / len(play):.1%} of gameplay")
        for value in sorted(counts):
            print(f"  {value} frames per tick: {counts[value] / len(paced):6.1%} "
                  f"of paced frames")

    tick = [f for f in play if f["ticks"] > 0]
    plain = [f for f in play if f["ticks"] == 0]
    print("\n== Frames that ran a simulation tick against the rest ==")
    print(f"tick frames: {len(tick)} ({len(tick) / len(play):.1%})")
    print("tick   frame ", describe(f["ms"] for f in tick))
    print("plain  frame ", describe(f["ms"] for f in plain))

    print("\n== Where a frame's time goes (ms) ==")
    for label, group in (("tick ", tick), ("plain", plain)):
        print(f"-- {label} frames --")
        if label == "tick ":
            print("  before the tick      ", describe(span(group, "start", "fixedBegin")))
            print("  fixed update         ", describe(span(group, "fixedBegin", "fixedEnd")))
            print("  variable update      ", describe(span(group, "fixedEnd", "variableEnd")))
            print("  update end to render ", describe(span(group, "variableEnd", "renderBegin")))
        else:
            print("  frame start to render", describe(span(group, "start", "renderBegin")))
        print("  pacer wait           ", describe(span(group, "renderBegin", "paceWaitEnd")))
        print("  plugin staging       ", describe(span(group, "paceWaitEnd", "renderOriginal")))
        print("  engine scene render  ", describe(span(group, "renderOriginal", "renderEnd")))
        print("  render end to swap   ", describe(span(group, "renderEnd", "swapEnter")))
        print("  wait before swap     ", describe(span(group, "swapEnter", "waitEnd")))
        print("  swap                 ",
              describe([f["ms"] - f["stamps"]["waitEnd"] for f in group
                        if f["stamps"]["waitEnd"] > 0]))
        print("  all work, no waits   ", describe(busy(group)))
        print("  view sampled before present",
              describe([f["ms"] - f["stamps"]["paceWaitEnd"] for f in group
                        if f["stamps"]["paceWaitEnd"] > 0]))

    print("\n== Pacing between ticks: mean frame time by position ==")
    by_position = {}
    position = None
    for f in play:
        position = 0 if f["ticks"] > 0 else \
            (None if position is None else position + 1)
        if position is not None and position < 10:
            by_position.setdefault(position, []).append(f["ms"])
    for position in sorted(by_position):
        values = by_position[position]
        if len(values) >= len(tick) // 20:
            label = "tick frame" if position == 0 else f"tick + {position}"
            print(f"  {label:<10} n={len(values):>6}  "
                  f"mean={sum(values) / len(values):6.3f}  "
                  f"p50={median(values):6.3f}")

    # How evenly the view turns. During a fast turn every frame should show
    # about the same step; a "gap" in a moving light is a step much larger
    # than its neighbours'.
    print("\n== View step evenness during fast turns ==")
    staged = [f for f in play if f["flags"] & 1]
    window = 12
    ratios_tick, ratios_plain = [], []
    steps = []
    for previous, f in zip(staged, staged[1:]):
        step = math.hypot(wrap(f["yaw"] - previous["yaw"]) *
                          math.cos(f["pitch"]), f["pitch"] - previous["pitch"])
        steps.append((f, step))
    for index in range(window, len(steps) - window):
        f, step = steps[index]
        near = steps[index - window:index + window + 1]
        total = sum(item[0]["ms"] for item in near)
        turned = sum(item[1] for item in near)
        if total <= 0.0 or total > 250.0:
            continue
        if turned / total * 1000.0 < math.radians(90.0):
            continue  # slower than 90 degrees per second
        middle = sorted(item[1] for item in near)[len(near) // 2]
        if middle <= 0.0:
            continue
        (ratios_tick if f["ticks"] else ratios_plain).append(step / middle)
    if len(ratios_tick) + len(ratios_plain) < 500:
        print("  not enough fast turning in this trace (needs an interpolated "
              "view and circles faster than 90 degrees per second)")
        return
    print("  step / median step of the 25 frames around it (1.0 = even):")
    print("    tick  frames", describe(ratios_tick))
    print("    plain frames", describe(ratios_plain))
    everything = ratios_tick + ratios_plain
    for threshold in (1.25, 1.5, 2.0):
        count = sum(1 for value in everything if value >= threshold)
        tick_count = sum(1 for value in ratios_tick if value >= threshold)
        print(f"  steps >= {threshold:.2f}x median: "
              f"{count / len(everything):6.2%} of frames; "
              f"{tick_count / max(1, count):.0%} of those are tick frames")


def main():
    arguments = sys.argv[1:]
    if arguments and arguments[0] == "--compare":
        if len(arguments) == 1:
            arguments += [str(BIN / "AmnesiaTheBunker.FPSFix.frames.baseline.bin"),
                          str(BIN / "AmnesiaTheBunker.FPSFix.frames.play.bin")]
        if len(arguments) != 3:
            sys.exit(__doc__)
        compare(Path(arguments[1]), Path(arguments[2]))
        return
    path = Path(arguments[0]) if arguments else \
        BIN / "AmnesiaTheBunker.FPSFix.frames.bin"
    report(path)


if __name__ == "__main__":
    main()
