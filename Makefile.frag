LLVM_LINK = $(llvm_bins_path)llvm-link
LLVM_OPT = $(llvm_bins_path)opt

$(srcdir)/phpllvm_execute.cpp: $(builddir)/module_template.bc
$(builddir)/zend_execute.bc.o: $(php_sources_path)/Zend/zend_execute.c $(php_sources_path)/Zend/zend_vm_execute.h

$(builddir)/preprocess_module_template: $(srcdir)/preprocess_module_template.cpp
	$(CXX) $(CXXFLAGS_CLEAN) $(COMMON_FLAGS) $< -o $@ $(LDFLAGS)

$(builddir)/phpllvm_runtime_helpers.bc.o: $(srcdir)/phpllvm_runtime_helpers.c
	$(LLVM_CC) -emit-llvm $(COMMON_FLAGS) $(CFLAGS_CLEAN) $(EXTRA_CFLAGS) -c $< -o $@
	$(LLVM_OPT) -std-compile-opts -f $@ -o $@

%.bc.o: $(php_sources_path)/Zend/%.c $(builddir)/preprocess_module_template
	$(LLVM_CC) -emit-llvm $(COMMON_FLAGS) $(CFLAGS_CLEAN) $(EXTRA_CFLAGS) -c $< -o $@
	$(builddir)/preprocess_module_template $@
	$(LLVM_OPT) -std-compile-opts -f $@ -o $@

$(builddir)/module_template.bc: $(builddir)/phpllvm_runtime_helpers.bc.o $(builddir)/zend_execute.bc.o $(builddir)/zend_execute_API.bc.o $(builddir)/zend_compile.bc.o $(builddir)/preprocess_module_template
	$(LLVM_LINK) $(builddir)/phpllvm_runtime_helpers.bc.o $(builddir)/zend_execute.bc.o $(builddir)/zend_execute_API.bc.o $(builddir)/zend_compile.bc.o > $@
	$(LLVM_OPT) -std-compile-opts -f $@ -o $@
	$(builddir)/preprocess_module_template $@ 2
