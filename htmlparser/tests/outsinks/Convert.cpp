/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *  
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *  
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 * 
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation. Portions created by Netscape are
 * Copyright (C) 1998-1999 Netscape Communications Corporation. All
 * Rights Reserved.
 * 
 * Contributor(s): Akkana Peck.
 */

#include <ctype.h>      // for isdigit()

#include "nsParserCIID.h"
#include "nsIParser.h"
#include "nsHTMLContentSinkStream.h"
#include "nsHTMLToTXTSinkStream.h"
#include "nsIComponentManager.h"
#include "CNavDTD.h"
#include "nsXIFDTD.h"

extern "C" void NS_SetupRegistry();

#ifdef XP_PC
#define PARSER_DLL "raptorhtmlpars.dll"
#endif
#ifdef XP_MAC
#endif
#if defined(XP_UNIX) || defined(XP_BEOS)
#define PARSER_DLL "libraptorhtmlpars"MOZ_DLL_SUFFIX
#endif

// Class IID's
static NS_DEFINE_IID(kParserCID, NS_PARSER_IID);

// Interface IID's
static NS_DEFINE_IID(kIParserIID, NS_IPARSER_IID);

nsresult
Compare(nsString& str, nsString& filename)
{
  printf("Sorry, don't know how to compare yet\n");
  char* charstar = str.ToNewUTF8String();
  printf("Output string is: %s\n-------------------- \n", charstar);
  delete[] charstar;
  return NS_ERROR_NOT_IMPLEMENTED;
}

//----------------------------------------------------------------------
// Convert html on stdin to either plaintext or (if toHTML) html
//----------------------------------------------------------------------
nsresult
HTML2text(nsString& inString, nsString& inType, nsString& outType, int wrapCol, nsString& compareAgainst)
{
  nsresult rv = NS_OK;

  nsString outString;

  // Create a parser
  nsIParser* parser;
   rv = nsComponentManager::CreateInstance(kParserCID, nsnull,
                                           kIParserIID,(void**)&parser);
  if (NS_FAILED(rv))
  {
    printf("Unable to create a parser : 0x%x\n", rv);
    return NS_ERROR_FAILURE;
  }

  nsIHTMLContentSink* sink = nsnull;

  // Create the appropriate output sink
  if (outType == "text/html")
    rv = NS_New_HTML_ContentSinkStream(&sink, &outString, 0);

  else  // default to plaintext
    rv = NS_New_HTMLToTXT_SinkStream(&sink, &outString, wrapCol, 2);

  if (NS_FAILED(rv))
  {
    printf("Couldn't create new content sink: 0x%x\n", rv);
    return rv;
  }

  parser->SetContentSink(sink);
  nsIDTD* dtd = nsnull;
  if (inType == "text/xif")
    rv = NS_NewXIFDTD(&dtd);
  else
    rv = NS_NewNavHTMLDTD(&dtd);
  if (NS_FAILED(rv))
  {
    printf("Couldn't create new HTML DTD: 0x%x\n", rv);
    return rv;
  }

  parser->RegisterDTD(dtd);

  char* inTypeStr = inType.ToNewCString();
  rv = parser->Parse(inString, 0, inTypeStr, PR_FALSE, PR_TRUE);
  delete[] inTypeStr;
  if (NS_FAILED(rv))
  {
    printf("Parse() failed! 0x%x\n", rv);
    return rv;
  }

  NS_IF_RELEASE(dtd);
  NS_IF_RELEASE(sink);
  NS_RELEASE(parser);

  if (compareAgainst.Length() > 0)
    return Compare(outString, compareAgainst);

  char* charstar = outString.ToNewUTF8String();
  printf("Output string is:\n--------------------\n%s\n--------------------\n",
         charstar);
  delete[] charstar;

  return NS_OK;
}

//----------------------------------------------------------------------

int main(int argc, char** argv)
{
  nsString inType ("text/html");
  nsString outType ("text/plain");
  int wrapCol = 72;
  nsString compareAgainst;


  // Skip over progname arg:
  const char* progname = argv[0];
  --argc; ++argv;

  // Process flags
  while (argc > 0 && argv[0][0] == '-')
  {
    switch (argv[0][1])
    {
      case 'h':
        printf("\
Usage: %s [-i intype] [-o outtype] [-w wrapcol] [-c comparison_file] infile\n\
\tIn/out types are mime types (e.g. text/html)\n\
\tcomparison_file is a file against which to compare the output\n\
\t  (not yet implemented\n\
\tDefaults are -i text/html -o text/plain -w 72 [stdin]\n",
               progname);
        exit(0);

        case 'i':
        if (argv[0][2] != '\0')
          inType = argv[0]+2;
        else {
          inType = argv[1];
          --argc;
          ++argv;
        }
        break;

      case 'o':
        if (argv[0][2] != '\0')
          outType = argv[0]+2;
        else {
          outType = argv[1];
          --argc;
          ++argv;
        }
        break;

      case 'w':
        if (isdigit(argv[0][2]))
          wrapCol = atoi(argv[0]+2);
        else {
          wrapCol = atoi(argv[1]);
          --argc;
          ++argv;
        }
        break;

      case 'c':
        if (argv[0][2] != '\0')
          compareAgainst = argv[0]+2;
        else {
          compareAgainst = argv[1];
          --argc;
          ++argv;
        }
        break;
    }
    ++argv;
    --argc;
  }

  FILE* file = 0;
  if (argc > 0)         // read from a file
  {
    // Open the file in a Unix-centric way,
    // until I find out how to use nsFileSpec:
    file = fopen(argv[0], "r");
    if (!file)
    {
      fprintf(stderr, "Can't open file %s", argv[0]);
      perror(" ");
      exit(1);
    }
  }
  else file = stdin;

  nsComponentManager::AutoRegister(nsIComponentManager::NS_Startup, 0);
  NS_SetupRegistry();

  // Read in the string: very inefficient, but who cares?
  nsString inString;
  char c;
  while ((c = getc(file)) != EOF)
    inString += c;

  if (file != stdin)
    fclose(file);

#if 0
  printf("Input string is: %s\n-------------------- \n",
         inString.ToNewCString());
  printf("------------------------------------\n");
#endif

  printf("inType = '%s', outType = '%s'\n", inType.ToNewCString(), outType.ToNewCString());
  HTML2text(inString, inType, outType, wrapCol, compareAgainst);

  return 0;
}
