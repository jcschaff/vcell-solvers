#ifndef ASTIDNODE_CPP
#define ASTIDNODE_CPP

#include "ASTIdNode.h"
#include "ExpressionException.h"
#include "ExpressionBindingException.h"
#include "RuntimeException.h"
#include "Expression.h"

ASTIdNode::ASTIdNode(int i) : SimpleNode(i) , symbolTableEntry(NULL)
{	
	symbolTableEntry = 0;
}

ASTIdNode::~ASTIdNode() {
}

string ASTIdNode::infixString(int lang, NameScope* nameScope)
{
	string idName(name);
	return idName;

    if (nameScope == null) {
        idName = name;
    } else {
	/*		
        if (symbolTableEntry != null) {
            idName = nameScope->getSymbolName(symbolTableEntry);
        } else {
            idName = nameScope->getUnboundSymbolName(name);
        }
    }
    if (lang == LANGUAGE_DEFAULT || lang == LANGUAGE_C) {
        return idName;
    } else if (lang == LANGUAGE_MATLAB) {
		return cbit.util.TokenMangler.getEscapedTokenMatlab(idName);
    } else if (lang == LANGUAGE_JSCL) {
        return cbit.util.TokenMangler.getEscapedTokenJSCL(idName);
    } else {        
	*/
		char chrs[20];
		sprintf(chrs, "%d\0", lang);
		throw RuntimeException(string("Lanaguage '") + chrs + " not supported");
	
	}	
}

void ASTIdNode::getStackElements(vector<StackElement>& elements) {
	elements.push_back(StackElement(symbolTableEntry->getValueProxy(), symbolTableEntry->getIndex()));
}

double ASTIdNode::evaluate(int evalType, double* values) {
	if (symbolTableEntry == null){
		throw ExpressionException("tryin to evaluate unbound identifier '" + infixString(LANGUAGE_DEFAULT, 0)+"'");
	}	

	if (evalType == EVALUATE_CONSTANT) {
		if (symbolTableEntry->isConstant()){
			return symbolTableEntry->getConstantValue();
		}		
		throw ExpressionException("Identifier '" + name + "' cannot be evaluated as a constant.");
	} else {
		Expression* exp = symbolTableEntry->getExpression();
		if (exp != null) {
			return exp->evaluateVector(values);
		} else {
			if (values == 0) {
				if (symbolTableEntry->getValueProxy() == NULL) {
					throw ExpressionException("Value proxy not found for indentifier '" + name + "'");
				}
				return symbolTableEntry->getValueProxy()->evaluate();
			} else {
				if (symbolTableEntry->getIndex() < 0) {
					throw ExpressionBindingException("referenced symbol table entry " + name + " not bound to an index");
				}
				return values[symbolTableEntry->getIndex()];
			}
		}
	}
}

SymbolTableEntry* ASTIdNode::getBinding(string symbol)
{
	if (name == symbol){
		return symbolTableEntry;
	}else{
		return null;
	}
}

void ASTIdNode::bind(SymbolTable* symbolTable)
{
	if (symbolTable == null){
		symbolTableEntry = null;
		return;
	}	

	symbolTableEntry = symbolTable->getEntry(name);

	if (symbolTableEntry == null){
		string id = name;
		throw ExpressionBindingException("error binding identifier '" + id + "'");
	}
}

void ASTIdNode::getSymbols(vector<string>& symbols, int language, NameScope* nameScope) {
	string infix = infixString(language, nameScope);
	for (int i = 0; i < (int)symbols.size(); i ++) {
		if (symbols[i] == infix) {
			return;
		}
	}
	symbols.push_back(infix);
}
#endif