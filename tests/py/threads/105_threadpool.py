# concurrent.futures.ThreadPoolExecutor
from concurrent.futures import ThreadPoolExecutor
with ThreadPoolExecutor(max_workers=4) as ex:
    print(list(ex.map(lambda x: x * x, range(10))))
