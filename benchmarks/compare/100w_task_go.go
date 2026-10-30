package main

import (
	"fmt"
	"sync"
	"time"
)

func main() {
	start := time.Now()
	nco := 10000000
	wg := sync.WaitGroup{}
	wg.Add(nco)
	// 100w task
	for i := 0; i < nco; i++ {
		go func(i int) {
			a := i + i
			_ = a
			wg.Done()
		}(i)
	}

	wg.Wait()

	elapsed := time.Since(start).Milliseconds()
	fmt.Printf("cost time: %d ms\n", elapsed)
}
