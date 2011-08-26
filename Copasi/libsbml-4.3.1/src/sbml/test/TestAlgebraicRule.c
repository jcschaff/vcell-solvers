/**
 * \file    TestAlgebraicRule.c
 * \brief   AlgebraicRule unit tests
 * \author  Ben Bornstein
 *
 * $Id: TestAlgebraicRule.c 13054 2011-02-26 00:32:46Z fbergmann $
 * $HeadURL: http://sbml.svn.sourceforge.net/svnroot/sbml/trunk/libsbml/src/sbml/test/TestAlgebraicRule.c $
 *
 * <!--------------------------------------------------------------------------
 * This file is part of libSBML.  Please visit http://sbml.org for more
 * information about SBML, and the latest version of libSBML.
 *
 * Copyright (C) 2009-2011 jointly by the following organizations: 
 *     1. California Institute of Technology, Pasadena, CA, USA
 *     2. EMBL European Bioinformatics Institute (EBML-EBI), Hinxton, UK
 *  
 * Copyright (C) 2006-2008 by the California Institute of Technology,
 *     Pasadena, CA, USA 
 *  
 * Copyright (C) 2002-2005 jointly by the following organizations: 
 *     1. California Institute of Technology, Pasadena, CA, USA
 *     2. Japan Science and Technology Agency, Japan
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.  A copy of the license agreement is provided
 * in the file named "LICENSE.txt" included with this software distribution
 * and also available online as http://sbml.org/software/libsbml/license.html
 * ---------------------------------------------------------------------- -->*/

#include <sbml/common/common.h>

#include <sbml/math/FormulaFormatter.h>
#include <sbml/math/FormulaParser.h>

#include <sbml/SBase.h>
#include <sbml/Rule.h>
#include <sbml/xml/XMLNamespaces.h>
#include <sbml/SBMLDocument.h>

#include <check.h>

#if __cplusplus
CK_CPPSTART
#endif

static Rule_t *AR;


void
AlgebraicRuleTest_setup (void)
{
  AR = Rule_createAlgebraic(2, 4);

  if (AR == NULL)
  {
    fail("AlgebraicRule_createAlgebraic() returned a NULL pointer.");
  }
}


void
AlgebraicRuleTest_teardown (void)
{
  Rule_free(AR);
}


START_TEST (test_AlgebraicRule_create)
{
  fail_unless( SBase_getTypeCode  ((SBase_t *) AR) == SBML_ALGEBRAIC_RULE );
  fail_unless( SBase_getMetaId    ((SBase_t *) AR) == NULL );
  fail_unless( SBase_getNotes     ((SBase_t *) AR) == NULL );
  fail_unless( SBase_getAnnotation((SBase_t *) AR) == NULL );

  fail_unless( Rule_getFormula((Rule_t *) AR) == NULL );
  fail_unless( Rule_getMath   ((Rule_t *) AR) == NULL );
}
END_TEST


START_TEST (test_AlgebraicRule_createWithFormula)
{
  const ASTNode_t *math;
  char *formula;

  Rule_t *ar = Rule_createAlgebraic(2, 4);
  Rule_setFormula(ar, "1 + 1");


  fail_unless( SBase_getTypeCode  ((SBase_t *) ar) == SBML_ALGEBRAIC_RULE );
  fail_unless( SBase_getMetaId    ((SBase_t *) ar) == NULL );

  math = Rule_getMath((Rule_t *) ar);
  fail_unless(math != NULL);

  formula = SBML_formulaToString(math);
  fail_unless( formula != NULL );
  fail_unless( !strcmp(formula, "1 + 1") );

  fail_unless( !strcmp(Rule_getFormula((Rule_t *) ar), formula) );

  Rule_free(ar);
  safe_free(formula);
}
END_TEST


START_TEST (test_AlgebraicRule_createWithMath)
{
  ASTNode_t       *math = SBML_parseFormula("1 + 1");
  Rule_t *ar   = Rule_createAlgebraic(2, 4);
  Rule_setMath(ar, math);


  fail_unless( SBase_getTypeCode  ((SBase_t *) ar) == SBML_ALGEBRAIC_RULE );
  fail_unless( SBase_getMetaId    ((SBase_t *) ar) == NULL );

  fail_unless( !strcmp(Rule_getFormula((Rule_t *) ar), "1 + 1") );
  fail_unless( Rule_getMath((Rule_t *) ar) != math );

  Rule_free(ar);
}
END_TEST


START_TEST (test_AlgebraicRule_free_NULL)
{
  Rule_free(NULL);
}
END_TEST


START_TEST (test_AlgebraicRule_createWithNS )
{
  XMLNamespaces_t *xmlns = XMLNamespaces_create();
  XMLNamespaces_add(xmlns, "http://www.sbml.org", "testsbml");
  SBMLNamespaces_t *sbmlns = SBMLNamespaces_create(2,3);
  SBMLNamespaces_addNamespaces(sbmlns,xmlns);

  Rule_t *r = 
    Rule_createAlgebraicWithNS(sbmlns);


  fail_unless( SBase_getTypeCode  ((SBase_t *) r) == SBML_ALGEBRAIC_RULE );
  fail_unless( SBase_getMetaId    ((SBase_t *) r) == NULL );
  fail_unless( SBase_getNotes     ((SBase_t *) r) == NULL );
  fail_unless( SBase_getAnnotation((SBase_t *) r) == NULL );

  fail_unless( SBase_getLevel       ((SBase_t *) r) == 2 );
  fail_unless( SBase_getVersion     ((SBase_t *) r) == 3 );

  fail_unless( Rule_getNamespaces     (r) != NULL );
  fail_unless( XMLNamespaces_getLength(Rule_getNamespaces(r)) == 2 );


  Rule_free(r);
}
END_TEST

START_TEST (test_AlgebraicRule_accessWithNULL)
{
  fail_unless (Rule_clone(NULL) == NULL);
  fail_unless (Rule_containsUndeclaredUnits(NULL) == 0);
  fail_unless (Rule_createAlgebraicWithNS(NULL) == NULL);
  fail_unless (Rule_createAssignmentWithNS(NULL) == NULL);
  fail_unless (Rule_createRateWithNS(NULL) == NULL);
  
  Rule_free(NULL);

  fail_unless (Rule_getDerivedUnitDefinition(NULL) == NULL);
  fail_unless (Rule_getFormula(NULL) == NULL);
  fail_unless (Rule_getL1TypeCode(NULL) == SBML_UNKNOWN);
  fail_unless (Rule_getMath(NULL) == NULL);
  fail_unless (Rule_getNamespaces(NULL) == NULL);
  fail_unless (Rule_getType(NULL) == RULE_TYPE_INVALID);
  fail_unless (Rule_getTypeCode(NULL) == SBML_UNKNOWN);
  fail_unless (Rule_getUnits(NULL) == NULL);
  fail_unless (Rule_getVariable(NULL) == NULL);
  fail_unless (Rule_isAlgebraic(NULL) == 0);
  fail_unless (Rule_isAssignment(NULL) == 0);
  fail_unless (Rule_isCompartmentVolume(NULL) == 0);
  fail_unless (Rule_isParameter(NULL) == 0);
  fail_unless (Rule_isRate(NULL) == 0);
  fail_unless (Rule_isScalar(NULL) == 0);
  fail_unless (Rule_isSetFormula(NULL) == 0);
  fail_unless (Rule_isSetMath(NULL) == 0);
  fail_unless (Rule_isSetUnits(NULL) == 0);
  fail_unless (Rule_isSetVariable(NULL) == 0);
  fail_unless (Rule_isSpeciesConcentration(NULL) == 0);
  fail_unless (Rule_setFormula(NULL, NULL) == LIBSBML_INVALID_OBJECT);
  fail_unless (Rule_setL1TypeCode(NULL, SBML_UNKNOWN) == LIBSBML_INVALID_OBJECT);
  fail_unless (Rule_setMath(NULL, NULL) == LIBSBML_INVALID_OBJECT);
  fail_unless (Rule_setUnits(NULL, NULL) == LIBSBML_INVALID_OBJECT);
  fail_unless (Rule_setVariable(NULL, NULL) == LIBSBML_INVALID_OBJECT);
  fail_unless (Rule_setVariable(NULL, NULL) == LIBSBML_INVALID_OBJECT);

}
END_TEST


Suite *
create_suite_AlgebraicRule (void)
{
  Suite *suite = suite_create("AlgebraicRule");
  TCase *tcase = tcase_create("AlgebraicRule");


  tcase_add_checked_fixture( tcase,
                             AlgebraicRuleTest_setup,
                             AlgebraicRuleTest_teardown );

  tcase_add_test( tcase, test_AlgebraicRule_create            );
  tcase_add_test( tcase, test_AlgebraicRule_createWithFormula );
  tcase_add_test( tcase, test_AlgebraicRule_createWithMath    );
  tcase_add_test( tcase, test_AlgebraicRule_free_NULL         );
  tcase_add_test( tcase, test_AlgebraicRule_createWithNS      );
  tcase_add_test( tcase, test_AlgebraicRule_accessWithNULL    );

  suite_add_tcase(suite, tcase);

  return suite;
}

#if __cplusplus
CK_CPPEND
#endif