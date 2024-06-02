

all: software-arch.png

%.svg: %.plantuml
	plantuml -tsvg $<

%.png: %.plantuml
	plantuml -tpng $<

.PHONY: all
