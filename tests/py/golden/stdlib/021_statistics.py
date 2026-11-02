"""L0 golden: statistics on fixed data sets."""
import statistics as st
from decimal import Decimal
from fractions import Fraction

data = [2, 4, 4, 4, 5, 5, 7, 9]
odd = [3, 1, 4, 1, 5, 9, 2]

print("mean", st.mean(data), st.mean(odd), repr(st.mean([1, 2])), repr(st.mean([1.5, 2.5])))
print("mean fraction", repr(st.mean([Fraction(1, 3), Fraction(2, 3)])))
print("mean decimal", repr(st.mean([Decimal("0.1"), Decimal("0.2")])))
print("fmean", st.fmean(data), st.fmean([1, 2, 3], weights=[3, 2, 1]))
print("geometric_mean", round(st.geometric_mean([1, 4, 16]), 9))
print("harmonic_mean", st.harmonic_mean([40, 60]), st.harmonic_mean([1, 0]))
print("median", st.median(data), st.median(odd), repr(st.median([1, 3])))
print("median_low/high", st.median_low(data), st.median_high(data), st.median_low([1, 3]), st.median_high([1, 3]))
print("median_grouped", st.median_grouped([1, 2, 2, 3, 4, 4, 4, 4, 4, 5]))
print("mode", st.mode(data), st.mode("aabbbc"), st.mode([1, 1, 2, 2]))
print("multimode", st.multimode([1, 1, 2, 2, 3]), st.multimode("aabbbcccc"), st.multimode([]))
print("pstdev", st.pstdev(data))
print("stdev", round(st.stdev(data), 12))
print("pvariance", st.pvariance(data), repr(st.pvariance([1, 2, 3, 4])))
print("variance", repr(st.variance(odd)), st.variance([Fraction(1), Fraction(2), Fraction(4)]))
print("variance with mean", st.variance([1, 2, 3], xbar=2))
print("quantiles", st.quantiles(range(1, 11)))
print("quantiles n=10", st.quantiles(range(1, 11), n=10))
print("quantiles inclusive", st.quantiles([1, 2, 3, 4, 5, 6, 7, 8], n=4, method="inclusive"))
print("quantiles n=2", st.quantiles([10, 20, 30], n=2))

x = [1, 2, 3, 4, 5]
y = [2, 4, 5, 4, 5]
print("covariance", st.covariance(x, y))
print("correlation", round(st.correlation(x, y), 12))
print("correlation ranked", st.correlation(x, [1, 3, 2, 5, 4], method="ranked"))
lr = st.linear_regression(x, y)
print("linear_regression", round(lr.slope, 12), round(lr.intercept, 12))
print("proportional", st.linear_regression(x, [2, 4, 6, 8, 10], proportional=True))

errors = [
    ("mean empty", lambda: st.mean([])),
    ("median empty", lambda: st.median([])),
    ("mode empty", lambda: st.mode([])),
    ("stdev one", lambda: st.stdev([1])),
    ("variance one", lambda: st.variance([5])),
    ("pstdev empty", lambda: st.pstdev([])),
    ("quantiles n=0", lambda: st.quantiles([1, 2], n=0)),
    ("quantiles one point", lambda: st.quantiles([1])),
    ("harmonic negative", lambda: st.harmonic_mean([1, -1])),
    ("geometric zero", lambda: st.geometric_mean([0, 1])),
    ("covariance lengths", lambda: st.covariance([1, 2], [1, 2, 3])),
    ("correlation constant", lambda: st.correlation([1, 1, 1], [1, 2, 3])),
]
for label, fn in errors:
    try:
        print(label, "->", fn())
    except st.StatisticsError as e:
        print(label, type(e).__name__, e)
print("StatisticsError is ValueError", issubclass(st.StatisticsError, ValueError))

nd = st.NormalDist(mu=100, sigma=15)
print("NormalDist", nd, nd.mean, nd.stdev, nd.variance)
print("cdf", round(nd.cdf(100), 12), round(nd.cdf(130), 9))
print("inv_cdf", round(nd.inv_cdf(0.5), 9), round(nd.inv_cdf(0.975), 6))
print("pdf", round(nd.pdf(100), 12))
print("zscore", nd.zscore(130))
print("quantiles", [round(q, 6) for q in nd.quantiles()])
print("overlap", round(st.NormalDist(0, 1).overlap(st.NormalDist(1, 1)), 9))
print("arith", nd + 10, nd * 2, nd - st.NormalDist(10, 20))
print("from_samples", st.NormalDist.from_samples([2, 4, 4, 4, 5, 5, 7, 9]))
print("eq", st.NormalDist(1, 2) == st.NormalDist(1, 2))
try:
    st.NormalDist(0, -1)
except st.StatisticsError as e:
    print("negative sigma", type(e).__name__, e)
print("kde", round(st.kde([1, 2, 3], h=1)(2), 9))
