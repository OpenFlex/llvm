/*
   +----------------------------------------------------------------------+
   | PHP LLVM extension                                                   |
   +----------------------------------------------------------------------+
   | Copyright (c) 2008-2012 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Joonas Govenius <joonas@php.net>                            |
   |          Nuno Lopes <nlopess@php.net>                                |
   +----------------------------------------------------------------------+
*/

#include "phpllvm_execute.h"
#include "phpllvm_compile.h"

#include <map>
#include <utility>

#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/PassManager.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Assembly/PrintModulePass.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Scalar.h>

// Including this header magically makes the JIT be linked in and register 
// itself at startup.
#include <llvm/ExecutionEngine/JIT.h>

using namespace llvm;
using namespace phpllvm;

// pointer to the original Zend engine execute function
typedef void (zend_execute_t)(zend_op_array *op_array TSRMLS_DC);
static zend_execute_t *old_execute = NULL;

std::map<void*, Function*> op_handlers;

static ExecutionEngine* engine;
static Module* module;
static FunctionPassManager* opt_fpass_manager;
static PassManager pass_manager;

static void optimize_function(Function* function) {
	verifyFunction(*function, AbortProcessAction);

#ifdef SLOW
	pass_manager.run(*module);
#endif
	opt_fpass_manager->run(*function);
}

void phpllvm::save_module(const char* filename) {
	verifyModule(*module, AbortProcessAction);

	std::string ErrorInfo;
	raw_fd_ostream os(filename, ErrorInfo);

	if (ErrorInfo.empty()) {
		WriteBitcodeToFile(module, os);
	}
}

void phpllvm::init_jit_engine(const char* filename) {
	InitializeNativeTarget();

	if (!filename) {
		filename = "module_template.bc";
	}

	// read in the template that includes the handlers
	OwningPtr<MemoryBuffer> buf;
	error_code code;
	std::string message;
	LLVMContext &context = getGlobalContext();

	code = MemoryBuffer::getFile(filename, buf);
	if (code) {
		zend_error_noreturn(E_ERROR, "phpllvm: couldn't read handlers file: %s", code.message().c_str());
	}

	if (!(module = ParseBitcodeFile(buf.get(), context, &message))) {
		zend_error_noreturn(E_ERROR, "phpllvm: couldn't parse handlers file: %s\n", message.c_str());
	}

	EngineBuilder builder(module);

#if 0
	// Disable frame pointer elimination
	TargetOptions opts;
	opts.NoFramePointerElim = true;
	builder.setTargetOptions(opts);
#endif

	builder.setEngineKind(EngineKind::JIT);
	builder.setErrorStr(&message);
	engine = builder.create();
	if (!engine) {
		zend_error_noreturn(E_ERROR, "phpllvm: could not create handlers module: %s\n", message.c_str());
	}

	// Force codegen of handlers. this is a workaround for an LLVM bug in the JIT engine
	for (Module::iterator I = module->begin(), E = module->end(); I != E; ++I) {
		Function *Fn = &*I;
		if (!Fn->isDeclaration() && Fn->getName().startswith("ZEND_")) {
			void * ptr = engine->getPointerToFunction(Fn);
			op_handlers.insert(std::make_pair(ptr, Fn));
		}
	}

	// Set up the optimization passes
	opt_fpass_manager = new FunctionPassManager(module);

	opt_fpass_manager->add(new TargetData(*engine->getTargetData()));
	pass_manager.add(new TargetData(*engine->getTargetData()));

	// IPO optimizations

	// Inline small opcode handlers and helper functions
	pass_manager.add(createFunctionInliningPass());
	// Run an IPO constant propagation pass. This removes a lot of the 
	// branching on opcode handler return values.
	pass_manager.add(createIPSCCPPass());

	// local optimizations
	opt_fpass_manager->add(createInstructionCombiningPass());
	opt_fpass_manager->add(createReassociatePass());
	opt_fpass_manager->add(createGVNPass());
	opt_fpass_manager->add(createCFGSimplificationPass());
}

void phpllvm::destroy_jit_engine() {
	delete engine;
	delete opt_fpass_manager;
}

void phpllvm::override_executor() {
	old_execute = zend_execute;
	zend_execute = phpllvm::execute;
}

void phpllvm::restore_executor() {
	if (old_execute) {
		zend_execute = old_execute;
	}
}

void phpllvm::execute(zend_op_array *op_array TSRMLS_DC) {

	if (EG(start_op)) {
		// This does not appear to be reachable, ext/readline just gives us interactive
		// op arrays in small compilable fragments, it doesn't append to the old one
		php_error(E_WARNING, "phpllvm: cannot execute interactive code");
		return;
	}

	/* Get/create the compiled function */

	char* name;
	bool cache;
	Function* function;

	if (!op_array->filename || std::string("Command line code") == op_array->filename) {
		/* Don't cache "Command line code". */
		name = estrdup("command_line_code");
		cache = false;
		function = NULL;

	} else {
		spprintf(&name, 0, "%s__c__%s__f__%s__s",
			(op_array->filename)? op_array->filename : "",
			(op_array->scope)? op_array->scope->name : "",
			(op_array->function_name)? op_array->function_name : "");

		cache = true;
		function = module->getFunction(name);
	}

	if (!function) {
		function = compile_op_array(op_array, name, module, engine TSRMLS_CC);

		if (!function) {
			/* Note that we can't even call old_execute because the template includes globals
			 	that are duplicates of the original executor's globals. Hence we can either
				use only old_execute or our execute. */
			zend_error_noreturn(E_ERROR, "phpllvm: couldn't compile function %s.\n", name);
		}

		optimize_function(function);
	}

	efree(name);

	/* Call the compiled function */
	std::vector<GenericValue> args;
	GenericValue val;

	val.PointerVal = op_array;
	args.push_back(val);

#ifdef ZTS
	// FIXME: this doesnt hit the fast case in LLVM's JIT
	val.PointerVal = TSRMLS_C;
	args.push_back(val);
#endif

	engine->runFunction(function, args);

	if (!cache) {
		engine->freeMachineCodeForFunction(function);
		// FIXME: needed? cast<Instruction>(function->use_begin().getUse().getUser())->eraseFromParent(); // delete the 'call' instruction
		function->eraseFromParent();
	}
}
