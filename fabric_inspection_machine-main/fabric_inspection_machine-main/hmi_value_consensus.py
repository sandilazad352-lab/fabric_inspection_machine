from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Iterable


@dataclass(frozen=True)
class NumericCandidate:
    value: int
    score: float
    strategy: str


@dataclass(frozen=True)
class ConsensusResult:
    value: int | None
    reason: str
    votes: dict[int, int]
    score_sum: dict[int, float]
    best_score: dict[int, float]


def choose_strict_numeric_value(
    candidates: Iterable[NumericCandidate],
    *,
    min_value: int = 0,
    max_value: int = 20,
    min_votes: int = 2,
) -> ConsensusResult:
    filtered = [candidate for candidate in candidates if min_value <= candidate.value <= max_value]
    if not filtered:
        return ConsensusResult(None, "no_valid_candidates", {}, {}, {})

    votes: Counter[int] = Counter()
    score_sum: defaultdict[int, float] = defaultdict(float)
    best_score: defaultdict[int, float] = defaultdict(float)
    strategies_by_value: defaultdict[int, set[str]] = defaultdict(set)

    for candidate in filtered:
        votes[candidate.value] += 1
        score_sum[candidate.value] += candidate.score
        strategies_by_value[candidate.value].add(candidate.strategy)
        if candidate.score > best_score[candidate.value]:
            best_score[candidate.value] = candidate.score

    ordered = sorted(
        votes,
        key=lambda value: (
            votes[value],
            len(strategies_by_value[value]),
            score_sum[value],
            best_score[value],
            value,
        ),
        reverse=True,
    )
    best_value = ordered[0]
    runner_up = ordered[1] if len(ordered) > 1 else None

    best_votes = votes[best_value]
    best_strategy_count = len(strategies_by_value[best_value])
    best_avg_score = score_sum[best_value] / best_votes
    best_peak_score = best_score[best_value]
    runner_votes = votes[runner_up] if runner_up is not None else 0
    runner_avg_score = (score_sum[runner_up] / runner_votes) if runner_up is not None else 0.0

    if best_votes < min_votes:
        return ConsensusResult(None, "insufficient_votes", dict(votes), dict(score_sum), dict(best_score))

    if best_strategy_count >= 3 and best_avg_score >= 0.25 and best_votes >= runner_votes + 1:
        return ConsensusResult(best_value, "strong_multi_strategy_consensus", dict(votes), dict(score_sum), dict(best_score))

    if best_votes >= 3 and best_strategy_count >= 2 and best_avg_score >= 0.2:
        return ConsensusResult(best_value, "multi_vote_consensus", dict(votes), dict(score_sum), dict(best_score))

    if runner_up is None:
        if best_votes >= 2 and best_avg_score >= 0.35:
            return ConsensusResult(best_value, "single_value_with_support", dict(votes), dict(score_sum), dict(best_score))
        return ConsensusResult(None, "single_value_not_strong_enough", dict(votes), dict(score_sum), dict(best_score))

    if best_votes >= runner_votes + 2 and best_avg_score >= 0.3 and best_peak_score >= runner_avg_score:
        return ConsensusResult(best_value, "clear_vote_gap", dict(votes), dict(score_sum), dict(best_score))

    return ConsensusResult(None, "ambiguous_candidates", dict(votes), dict(score_sum), dict(best_score))
