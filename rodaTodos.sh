
#!/bin/bash

make
for T in {1..8}
do
    echo "================================="
    echo "THREADS = $T"
    echo "================================="

    ./teste $1 1024 256 $T 10 -tb2
done
```
