# Build the RPCEmu host interfaces manual.
#
# The sources are PRM-in-XML; riscos-prminxml renders them. CI builds the same
# manual through gerph/riscos-prminxml-action and publishes it with each
# release, so this is for working on it locally.
#
#   make lint      check every chapter
#   make html      render each chapter on its own
#   make indexed   render the whole manual, with contents and the SWI index
#   make           all three
#   make pdf       and a PDF, if Prince is installed
#
# Output goes to prm-output/ at the top of the tree, which is gitignored.
#
# Chapters are found by wildcard, so adding one means adding the file and
# listing it in index.xml - nothing here needs changing. The numeric prefixes
# are for the reader; index.xml is what actually fixes the order.

OUTPUT = ../../prm-output

# Every document except index.xml, which is the manifest rather than a chapter.
CHAPTERS = $(filter-out index.xml, \
             $(wildcard *.xml) $(wildcard front/*.xml) $(wildcard appendix/*.xml))

# riscos-prminxml flattens its output, so front/title-page.xml becomes
# title-page.html at the top of the output directory.
HTML   = $(patsubst %.xml,$(OUTPUT)/%.html,$(notdir $(CHAPTERS)))
STAMPS = $(patsubst %.xml,$(OUTPUT)/lint/%.stamp,$(notdir $(CHAPTERS)))

# So the pattern rules can find a chapter without caring which directory it
# lives in.
vpath %.xml . front appendix

.PHONY: all lint html lint-indexed indexed pdf clean

all: lint html indexed

lint: $(STAMPS)

html: $(HTML)

$(OUTPUT)/lint/%.stamp: %.xml
	@mkdir -p $(dir $@)
	riscos-prminxml -f lint $<
	@touch $@

$(OUTPUT)/%.html: %.xml
	@mkdir -p $(OUTPUT)
	riscos-prminxml -f html5+xml -O $(OUTPUT) $<

# The indexed build takes its paths from index.xml, and depends on every
# chapter because any of them can change what the contents and the SWI index
# contain.
lint-indexed: $(CHAPTERS) index.xml
	@mkdir -p $(OUTPUT)/indexed/logs
	riscos-prminxml --lint -f index -L $(OUTPUT)/indexed/logs index.xml

indexed: $(CHAPTERS) index.xml
	@mkdir -p $(OUTPUT)/indexed/logs
	riscos-prminxml -f index -L $(OUTPUT)/indexed/logs index.xml

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
