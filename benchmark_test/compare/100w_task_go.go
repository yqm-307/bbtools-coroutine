package main

import (
	"sync"
	"time"
	"fmt"
)

func main() {
	start := time.Now()

	wg := sync.WaitGroup{}
	wg.Add(1000000)
	// 100w task
	for i := 0; i < 1000000; i++ {
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
