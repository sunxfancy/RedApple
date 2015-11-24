/* 
* @Author: sxf
* @Date:   2015-10-26 14:00:25
* @Last Modified by:   sxf
* @Last Modified time: 2015-11-24 14:26:44
*/

#include "CodeGenContext.h"
#include "StringNode.h"
#include "StructModel.h"
#include "MetaModel/StructModel.h"
#include "MetaModel/FunctionModel.h"
#include "nodes.h"
#include <stdio.h>

static Value* function_macro(CodeGenContext* context, Node* node) {
	// 第二个参数, 函数名
	node = node->getNext();
	std::string function_name = node->getStr();
	FunctionModel* fm = context->getFunctionModel(function_name);
	fm->genMetaCode(context);
	Function* F = fm->getFunction(context);

	// 第三个参数, 参数表
	Node* args_node = node = node->getNext();
	vector<string>& arg_name = fm->name_list;

	// 第四个参数, 代码块
	node = node->getNext();
	if (node->getChild() == NULL) {
		errs() << "function:" << function_name << '\n';
		return F;
	}
	BasicBlock* bb = context->createBlock(F); // 创建新的Block

	// 特殊处理参数表, 这个地方特别坑，你必须给每个函数的参数
	// 手动AllocaInst开空间，再用StoreInst存一遍才行，否则一Load就报错
	// context->MacroMake(args_node->getChild());
	if (args_node->getChild() != NULL) {
		context->MacroMake(args_node);
		int i = 0;
		for (auto arg = F->arg_begin(); i != arg_name.size(); ++arg, ++i) {
			arg->setName(arg_name[i]);
			Value* argumentValue = arg;
			ValueSymbolTable* st = bb->getValueSymbolTable();
			Value* v = st->lookup(arg_name[i]);
			new StoreInst(argumentValue, v, false, bb);
		}
	}
	context->MacroMake(node);

	// 处理块结尾
	bb = context->getNowBlock();
	if (bb->getTerminator() == NULL)
		ReturnInst::Create(*(context->getContext()), bb);
	return F;
}

static Value* set_macro(CodeGenContext* context, Node* node) {
	// 参数一 类型
	Type* t;
	if (node->isTypeNode()) {
		TypeNode* tn = (TypeNode*) node;
		t = tn->typeGen(context);
	} else printf("错误的节点类型\n");

	// 参数二 变量名
	Node* var_node = node->getNext();
	const char* var_name = ((StringNode*)var_node)->getStr().c_str();
	AllocaInst *alloc = new AllocaInst(t, var_name, context->getNowBlock());

	// 参数三 初始化值
	// 这里有个问题 初始化为常数时,Store会出问题
	node = var_node->getNext();
	if (node == NULL) return alloc;
	Value* init_expr = node->codeGen(context);
	if (init_expr == NULL) {
		errs() << "变量的初始化无效: " << var_name << "\n";
		exit(1);
	}
	new StoreInst(init_expr, alloc, false, context->getNowBlock());
	return alloc;
}

static Value* select_macro(CodeGenContext* context, Node* node) {
	bool save_bool = false;
	if (context->isSave()) {
		save_bool = true;
		context->setIsSave(false);
	}

	Value* value = node->codeGen(context);
	Type* t_i32 = IntegerType::getInt32Ty(*(context->getContext()));
	Constant* zero = Constant::getNullValue(t_i32);
	BasicBlock* bb = context->getNowBlock();

	std::vector<Value*> args; 
	for (Node* p = node->getNext(); p != NULL; p = p->getNext()) {
		Value* v = p->codeGen(context);
		if (v != NULL) {
			args.push_back(v);
		}
	}

	Value* ptr = GetElementPtrInst::Create(value, args, "", context->getNowBlock());
	if (save_bool) return ptr;
	return new LoadInst(ptr, "", false, context->getNowBlock());		
}

static Value* call_macro(CodeGenContext* context, Node* node) {
	// 参数一 函数名
	Value* func = context->getFunction(node);
	if (func == NULL) {
		errs() <<  "找不到函数的定义：";
		errs() << ((StringNode*)node)->getStr().c_str() << "\n";
		exit(1);
	}

	// 其余参数 要传入的参数
	std::vector<Value*> args;
	for (Node* p = node->getNext(); p != NULL; p = p->getNext()) {
		Value* v = p->codeGen(context);
		if (v != NULL)
			args.push_back(v);
	}

	CallInst *call = CallInst::Create(func, args, "", context->getNowBlock());
	return call;
}

static Value* for_macro(CodeGenContext* context, Node* node) {
	// 参数一 初始化
	BasicBlock* init_block   = context->getNowBlock();
	context->MacroMake(node);

	// 参数二 终止条件
	node = node->getNext();
	BasicBlock* end_block = context->createBlock();
	Value* condition = context->MacroMake(node);

	// 参数三 每次循环
	node = node->getNext();
	BasicBlock* do_block = context->createBlock();
	context->MacroMake(node);

	// 参数四 循环体
	node = node->getNext();
	BasicBlock* work_block = context->createBlock();
	context->MacroMake(node);

	// 生成for循环
	BasicBlock* false_block = context->createBlock();
	BranchInst* branch      = BranchInst::Create(work_block, false_block, condition, end_block);
	// BranchInst::Create(init_block, father_block);
	BranchInst::Create(end_block, init_block);
	BranchInst::Create(do_block,   work_block);
	BranchInst::Create(end_block,  do_block);

	return branch;
}

static Value* while_macro(CodeGenContext* context, Node* node) {
	// 参数一 条件
	BasicBlock* father_block = context->getNowBlock();
	BasicBlock* pd_block     = context->createBlock();
	Value* condition = context->MacroMake(node);

	// 参数二 循环体
	node = node->getNext();
	BasicBlock* true_block = context->createBlock();
	context->MacroMake(node);

	// 生成while循环
	BasicBlock* false_block = context->createBlock();
	BranchInst* branch      = BranchInst::Create(true_block, false_block, condition, pd_block);
	BranchInst::Create(pd_block, father_block);
	BranchInst::Create(pd_block, true_block);

	return branch;
}

static Value* if_macro(CodeGenContext* context, Node* node) {
	// 参数一 条件
	Value* condition = context->MacroMake(node);
	BasicBlock* father_block = context->getNowBlock();

	// 参数二 为真时, 跳转到的Label
	node = node->getNext();

	BasicBlock* true_block = context->createBlock();
	context->MacroMake(node);

	// 参数三 为假时, 跳转到的Label
	node = node->getNext();
	BasicBlock* false_block = context->createBlock();
	if (node != NULL) {
		context->MacroMake(node);
		BasicBlock* end_block = context->createBlock();
		BranchInst::Create(end_block, true_block);
		BranchInst::Create(end_block, false_block);
	} else {
		BranchInst::Create(false_block, true_block);
	}

	BranchInst* branch = BranchInst::Create(true_block, false_block, condition, father_block);

	return branch;
}




static Value* opt1_macro(CodeGenContext* context, Node* node) {
	std::string opt = node->getStr();

	node = node->getNext();
	if (node == NULL) return NULL;
	context->setIsSave(true); // 这两句设置的目前是为下面的节点解析时,返回指针而不是load后的值
	Value* ans = node->codeGen(context);
	context->setIsSave(false);
	AtomicRMWInst::BinOp bop; 
	Value* one = ConstantInt::get(Type::getInt64Ty(*(context->getContext())), 1); 
	if (opt == "~") { return BinaryOperator::CreateNot(ans, "", context->getNowBlock()); }
	if (opt == "++") { bop = AtomicRMWInst::BinOp::Add;  goto selfWork; }
	if (opt == "--") { bop = AtomicRMWInst::BinOp::Sub;  goto selfWork; }
	if (opt == "b++") { bop = AtomicRMWInst::BinOp::Add; goto saveWork; }
	if (opt == "b--") { bop = AtomicRMWInst::BinOp::Sub; goto saveWork; }

	return NULL;

selfWork:
	new AtomicRMWInst(bop, ans, one, AtomicOrdering::SequentiallyConsistent, 
		SynchronizationScope::CrossThread, context->getNowBlock());
	return new LoadInst(ans, "", false, context->getNowBlock());	

saveWork:
	return new AtomicRMWInst(bop, ans, one, AtomicOrdering::SequentiallyConsistent, 
		SynchronizationScope::CrossThread, context->getNowBlock());
}

static Value* getCast(Value* v, Type* t, BasicBlock* bb) {
	Instruction::CastOps cops = CastInst::getCastOpcode(v, true, t, true);
	CastInst::Create(cops, v, t, "", bb);
}

static void normalize_type(Value*& v1, Value*& v2, BasicBlock* bb) {
	Type* t1 = v1->getType();
	Type* t2 = v2->getType();
	if (t1->isDoubleTy() || t2->isDoubleTy()) {
		if (!t1->isDoubleTy()) v1 = getCast(v1, t2, bb);
		if (!t2->isDoubleTy()) v2 = getCast(v2, t1, bb);
		return;
	}
	if (t1->isFloatTy() || t2->isFloatTy()) {
		if (!t1->isFloatTy()) v1 = getCast(v1, t2, bb);
		if (!t2->isFloatTy()) v2 = getCast(v2, t1, bb);
		return;
	}
}

static Value* opt2_macro(CodeGenContext* context, Node* node) {
	std::string opt = node->getStr();

	Node* op1 = (node = node->getNext());
	if (node == NULL) return NULL;
	Node* op2 = (node = node->getNext());
	if (node == NULL) return NULL;


	if (opt == "=") {
		context->setIsSave(true); // 这两句设置的目前是为下面的节点解析时,返回指针而不是load后的值
		Value* ans1 = op1->codeGen(context);
		context->setIsSave(false);
		Value* ans2 = op2->codeGen(context);
		return new StoreInst(ans2, ans1, false, context->getNowBlock());
	}
	AtomicRMWInst::BinOp bop; 
	if (opt == "+=") { bop = AtomicRMWInst::BinOp::Add; goto rmwOper; }
	if (opt == "-=") { bop = AtomicRMWInst::BinOp::Sub; goto rmwOper; }
	if (opt == "&=") { bop = AtomicRMWInst::BinOp::And; goto rmwOper; }
	if (opt == "|=") { bop = AtomicRMWInst::BinOp::Or;  goto rmwOper; }
	if (opt == "^=") { bop = AtomicRMWInst::BinOp::Xor; goto rmwOper; }
	goto Next;
rmwOper:{
	context->setIsSave(true);
	Value* ans1 = op1->codeGen(context);
	context->setIsSave(false);
	Value* ans2 = op2->codeGen(context);
	return new AtomicRMWInst(bop, ans1, ans2, AtomicOrdering::SequentiallyConsistent, 
		SynchronizationScope::CrossThread, context->getNowBlock());
}
Next:

	if (opt == ".") {
		bool save_bool = false;
		if (context->isSave()) {
			save_bool = true;
			context->setIsSave(false);
		}

		Value* ans1 = op1->codeGen(context);
		StringNode* sn = (StringNode*)op2;
		string ans2 = sn->getStr();
		Type* ans1_type = ans1->getType();
		if (!ans1_type->getPointerElementType()->isStructTy()) {
			errs() << "‘.’运算前，类型错误： " << *(ans1_type) << "\n";
			return NULL;
		}
		string struct_name = ans1_type->getPointerElementType()->getStructName();
		id* i = context->st->find(struct_name);
		if (i == NULL)  {
			errs() << "符号未找到: " << struct_name << "\n"; 
		}
		if (i->type != struct_t) {
			errs() << "‘.’运算前，符号表错误\n"; 
		}
		StructModel* sm = (StructModel*)(i->data);
		int n = sm->find(ans2);
 
		ConstantInt* zero = ConstantInt::get(Type::getInt32Ty(*(context->getContext())), 0);
		ConstantInt* num = ConstantInt::get(Type::getInt32Ty(*(context->getContext())), n);
    	std::vector<Value*> indices;
    	indices.push_back(zero); 
    	indices.push_back(num);

		GetElementPtrInst* ptr = GetElementPtrInst::Create(ans1, indices, "", context->getNowBlock());
		if (save_bool) return ptr;
		return new LoadInst(ptr, "", false, context->getNowBlock());			
	}

	Value* ans1 = op1->codeGen(context);
	Value* ans2 = op2->codeGen(context);

	Instruction::BinaryOps instr;
	if (ans1->getType()->isIntegerTy() && ans2->getType()->isIntegerTy() ) { // 还需考虑整数位长度
		if (opt == "+") { instr = Instruction::Add;  goto binOper; }
		if (opt == "-") { instr = Instruction::Sub;  goto binOper; }
		if (opt == "*") { instr = Instruction::Mul;  goto binOper; }
		if (opt == "/") { instr = Instruction::SDiv; goto binOper; }
		if (opt == "<<") { instr = Instruction::Shl; goto binOper; }
		if (opt == ">>") { instr = Instruction::LShr; goto binOper; } // 注意，这里没处理有符号数和无符号数的问题 AShr(arithmetic)
		if (opt == "&") { instr = Instruction::And; goto binOper; }
		if (opt == "|") { instr = Instruction::Or; goto binOper; }
		if (opt == "^") { instr = Instruction::Xor; goto binOper; }
	} else {
		normalize_type(ans1, ans2, context->getNowBlock());
		if (opt == "+") { instr = Instruction::FAdd;  goto binOper; }
		if (opt == "-") { instr = Instruction::FSub;  goto binOper; }
		if (opt == "*") { instr = Instruction::FMul;  goto binOper; }
		if (opt == "/") { instr = Instruction::FDiv; goto binOper; }
	}

	CmpInst::Predicate instp;
	if (ans1->getType()->isIntegerTy() && ans2->getType()->isIntegerTy() ) {
		if (opt == "==") { instp = CmpInst::Predicate::ICMP_EQ;  goto cmpOper; } 
		if (opt == "!=") { instp = CmpInst::Predicate::ICMP_NE;  goto cmpOper; } 
		if (opt == "<=") { instp = CmpInst::Predicate::ICMP_SLE; goto cmpOper; } 
		if (opt == ">=") { instp = CmpInst::Predicate::ICMP_SGE; goto cmpOper; } 
		if (opt == "<")  { instp = CmpInst::Predicate::ICMP_SLT; goto cmpOper; } 
		if (opt == ">")  { instp = CmpInst::Predicate::ICMP_SGT; goto cmpOper; } 
	}
	return NULL;

binOper:
	return BinaryOperator::Create(instr, ans1, ans2, "", context->getNowBlock());

cmpOper:
	return CmpInst::Create(Instruction::ICmp, instp, ans1, ans2, "", context->getNowBlock());
}

static Value* struct_macro(CodeGenContext* context, Node* node) {
	std::string struct_name = node->getStr();
	StructModel* sm = context->getStructModel(struct_name);
	sm->genMetaCode(context);
	return NULL;
}

static Value* return_macro(CodeGenContext* context, Node* node) {
	Value* v = node->codeGen(context);
	return ReturnInst::Create(*(context->getContext()), v, context->getNowBlock());
}

static Value* new_macro(CodeGenContext* context, Node* node) {
	Type* ITy = Type::getInt64Ty(*(context->getContext()));
	TypeNode* tn = (TypeNode*) node;
	Type* t = context->FindSrcType(tn->getTypeName());

	// 第二个参数，构造函数表
	node = node->getNext();

	// 第三个参数，维度信息
	vector<Value*> args;
	node = node->getNext();
	if (node != NULL)
		for (Node* p = node->getChild(); p != NULL; p = p->getNext()) {
			args.push_back(p->codeGen(context));
		}
	Constant* AllocSize = ConstantExpr::getSizeOf(t);
	BasicBlock* bb = context->getNowBlock();
	if (args.size() == 0) {
		Instruction* Malloc = CallInst::CreateMalloc(bb, ITy, t, AllocSize);
		Malloc->insertAfter(&(bb->back()));
		return Malloc;
	} else {
		// 这里实现自定义的数组malloc函数
		ConstantInt* zero = ConstantInt::get(Type::getInt64Ty(*(context->getContext())), 0);
		args.push_back(zero);
		string func_name = "malloc_array";
		CallInst *call = CallInst::Create(context->getFunction(func_name), 
			args, "", bb);
		// t = ArrayType::get(t, 0);
		t = t->getPointerTo();
		return CastInst::CreatePointerCast(call, t, "", bb);
	}
}

extern Node* parseFile(const char* path);

static Value* import_macro(CodeGenContext* context, Node* node) {
	string file_name = node->getStr();
	context->SaveMacros();
	context->ScanOther(parseFile(file_name.c_str()));
	context->RecoverMacros();
	return NULL;
}

extern const FuncReg macro_funcs[] = {
	{"function", function_macro},
	{"struct",   struct_macro},
	{"set",      set_macro},
	{"call",     call_macro},
	{"select",   select_macro},
	{"opt1",     opt1_macro},
	{"opt2",     opt2_macro},
	{"for",      for_macro},
	{"while",    while_macro},
	{"if",       if_macro},
	{"return",   return_macro},
	{"new",      new_macro},
	{"import",   import_macro}, // 实验型导入功能,最后应从库中删除 
	{NULL, NULL}
};