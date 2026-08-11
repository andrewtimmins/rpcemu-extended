# Build the RPCEmu host interfaces manual.
#
# The sources are PRM-in-XML; riscos-prminxml renders them. CI does the same
# thing through gerph/riscos-prminxml-action and publishes the result with each
# release, so this Makefile is for working on the manual locally.
#
#   make lint      check every chapter
#   make html      render each chapter on its own
#   make indexed   render the whole thing as one navigable manual
#   make           all three
#
# Output goes to prm-output/ at the top of the tree, which is gitignored.

OUTPUT = ../../prm-output

# Explicit, and in reading order: the index lists these chapters in this
# sequence and the manual is meant to be read that way.
XMLS = \
	front/title-page.xml \
	01-overview.xml \
	02-hostfs.xml \
	03-hostcmd.xml \
	04-clipboard.xml \
	05-network.xml \
	appendix/a1-reserved.xml

.PHONY: all lint html lint-indexed indexed pdf clean

all: lint html indexed

lint:
	@for file in $(XMLS); do \
		echo "Linting $$file"; \
		riscos-prminxml -f lint "$$file" || exit 1; \
	done

html:
	@mkdir -p $(OUTPUT)
	@for file in $(XMLS); do \
		echo "Building $$file"; \
		riscos-prminxml -f html5+xml -O $(OUTPUT) "$$file" || exit 1; \
	done

lint-indexed:
	@mkdir -p $(OUTPUT)/indexed/logs
	@echo "Linting the indexed manual"
	@riscos-prminxml --lint -f index -L $(OUTPUT)/indexed/logs index.xml

indexed:
	@mkdir -p $(OUTPUT)/indexed/logs
	@echo "Building the indexed manual"
	@riscos-prminxml -f index -L $(OUTPUT)/indexed/logs index.xml

# Prince is not assumed to be installed: it is a commercial tool and nothing
# else here needs it.
pdf: indexed
	@if command -v prince >/dev/null 2>&1; then \
		cd $(OUTPUT)/indexed/pages && prince -l filelist.txt -o RPCEmuHostInterfaces.pdf; \
	else \
		echo "Prince is not installed; skipping PDF generation"; \
	fi

clean:
	rm -rf $(OUTPUT)
