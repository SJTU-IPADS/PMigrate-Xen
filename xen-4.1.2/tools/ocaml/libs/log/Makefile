TOPLEVEL=$(CURDIR)/../..
XEN_ROOT=$(TOPLEVEL)/../..
include $(TOPLEVEL)/common.make

OBJS = syslog log logs
INTF = log.cmi logs.cmi syslog.cmi
LIBS = log.cma log.cmxa

all: $(INTF) $(LIBS) $(PROGRAMS)

bins: $(PROGRAMS)

libs: $(LIBS)

log.cmxa: libsyslog_stubs.a $(foreach obj,$(OBJS),$(obj).cmx)
	$(call mk-caml-lib-native, $@, -cclib -lsyslog_stubs, $(foreach obj,$(OBJS),$(obj).cmx))

log.cma: $(foreach obj,$(OBJS),$(obj).cmo)
	$(call mk-caml-lib-bytecode, $@, -dllib dllsyslog_stubs.so -cclib -lsyslog_stubs, $(foreach obj,$(OBJS),$(obj).cmo))

syslog_stubs.a: syslog_stubs.o
	$(call mk-caml-stubs, $@, $+)

libsyslog_stubs.a: syslog_stubs.o
	$(call mk-caml-lib-stubs, $@, $+)

logs.mli : logs.ml
	$(OCAMLC) -i $(OCAMLCFLAGS) $< > $@

syslog.mli : syslog.ml
	$(OCAMLC) -i $< > $@

.PHONY: install
install: $(LIBS) META
	mkdir -p $(OCAMLDESTDIR)
	ocamlfind remove -destdir $(OCAMLDESTDIR) log
	ocamlfind install -destdir $(OCAMLDESTDIR) -ldconf ignore log META $(INTF) $(LIBS) *.a *.so *.cmx

.PHONY: uninstall
uninstall:
	ocamlfind remove -destdir $(OCAMLDESTDIR) log

include $(TOPLEVEL)/Makefile.rules

