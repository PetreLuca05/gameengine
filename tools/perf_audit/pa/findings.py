"""Findings: what a rule reports, how findings merge across scenes and how they rank."""
import hashlib

EFFORT_WEIGHT = {"S": 1.0, "M": 3.0, "L": 8.0}
CONFIDENCE_WEIGHT = {"high": 0.9, "medium": 0.6, "low": 0.3}


class Finding:
    def __init__(self, rule, key, title, system, location, evidence, savings, confidence, effort, fix,
                 fidelity="none", verify=None, scene=None, source="runtime", fix_id=None, below_noise=False):
        self.rule = rule
        self.key = key                      # stable within the rule: location identity
        self.title = title
        self.system = system
        self.location = location            # {"pass"/"function"/"file"/"zone": ...}
        self.evidence = evidence            # numbers, per scene
        self.savings = {"cpu_ms": 0.0, "gpu_ms": 0.0, "mb": 0.0, "bytes_s": 0.0}
        self.savings.update(savings or {})
        self.confidence = confidence        # high / medium / low
        self.effort = effort                # S / M / L
        self.fix = fix
        self.fidelity = fidelity            # "none" (main list) or "tradeoff" (listed, not recommended)
        self.fix_id = fix_id                # PF-xxxx when implemented (perf_fix_disable toggles it)
        if verify is None:
            verify = "perf_verify %s (perf_fix_disable A/B)" % fix_id if fix_id else "verify by image + state diff after implementation (perf_verify)"
        self.verify = verify
        self.scenes = [scene] if scene else []
        self.source = source                # runtime / static / inspection
        self.below_noise = below_noise

    @property
    def id(self):
        return "F-" + hashlib.sha1(("%s|%s" % (self.rule, self.key)).encode()).hexdigest()[:6]

    def score(self):
        s = self.savings
        value = s["cpu_ms"] + s["gpu_ms"] + 0.02 * s["mb"] + s["bytes_s"] / 50000.0
        value = max(value, 0.001)
        if self.below_noise:
            value *= 0.1
        return value * CONFIDENCE_WEIGHT.get(self.confidence, 0.3) / EFFORT_WEIGHT.get(self.effort, 3.0)

    def merge(self, other):
        """The same finding in another scene: keep the largest savings, collect evidence."""
        for k, v in other.savings.items():
            self.savings[k] = max(self.savings.get(k, 0.0), v)
        for sc in other.scenes:
            if sc not in self.scenes:
                self.scenes.append(sc)
        for k, v in other.evidence.items():
            if k not in self.evidence:
                self.evidence[k] = v
        self.below_noise = self.below_noise and other.below_noise

    def to_dict(self):
        return {
            "id": self.id, "rule": self.rule, "title": self.title, "system": self.system, "location": self.location,
            "evidence": self.evidence, "savings": self.savings, "confidence": self.confidence, "effort": self.effort,
            "fix": self.fix, "fidelity_impact": self.fidelity, "verify": self.verify, "fix_id": self.fix_id,
            "scenes": self.scenes, "source": self.source, "below_noise": self.below_noise, "score": round(self.score(), 5),
        }


def merge_all(findings):
    by_id = {}
    for f in findings:
        if f.id in by_id:
            by_id[f.id].merge(f)
        else:
            by_id[f.id] = f
    return list(by_id.values())


def ranked(findings):
    return sorted(findings, key=lambda f: (-f.score(), f.rule, f.key))
