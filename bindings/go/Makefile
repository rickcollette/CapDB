.PHONY: test build clean help

CAPDB_BUILD ?= ../../build

help:
	@echo "CapDB Go Driver - Build targets:"
	@echo "  make test       - Run driver tests"
	@echo "  make build      - Build example application"
	@echo "  make clean      - Clean build artifacts"

test:
	@eval $$(../../capdb-go-build-env.sh $(CAPDB_BUILD)) && \
	go test -tags capdb -v ./capdb

build: test
	@eval $$(../../capdb-go-build-env.sh $(CAPDB_BUILD)) && \
	go build -tags capdb -o example/capdb-example ./example

clean:
	rm -f example/capdb-example
	go clean -testcache

.PHONY: fmt lint

fmt:
	gofmt -w .

lint: fmt
	go vet -tags capdb ./...
