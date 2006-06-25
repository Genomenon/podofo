/***************************************************************************
 *   Copyright (C) 2005 by Dominik Seichter                                *
 *   domseichter@web.de                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "PdfWriter.h"

#include "PdfObject.h"
#include "PdfParser.h"
#include "PdfVariant.h"

//#define PDF_MAGIC "%����\n" //"%\0x25\0xe2\0xe3\0xcf\0xd3\0x0d"
#define PDF_MAGIC           "\xe2\xe3\xcf\xd3\n"
#define EMPTY_OBJECT_OFFSET 65535
#define XREF_ENTRY_SIZE     20

namespace PoDoFo {

PdfWriter::PdfWriter()
{
    Clear();
}

PdfWriter::~PdfWriter()
{
    Clear();
}

void PdfWriter::Clear() 
{
    TIVecObjects it = m_vecObjects.begin();

    while( it != m_vecObjects.end() )
    {
        delete (*it);
        ++it;
    }

    m_vecObjects.clear();

    m_pParser      = NULL;
    m_eVersion     = ePdfVersion_1_3;

    m_pCatalog     = NULL;
    m_pInfo        = NULL;

    m_bCompress    = true;
}

PdfError PdfWriter::Init()
{
    PdfError eCode;

    // clear everything - so that calling Init twice will work
    Clear();

    m_pInfo     = m_vecObjects.CreateObject( NULL );
    m_pCatalog  = m_vecObjects.CreateObject( "Catalog" );

    return eCode;
}

PdfError PdfWriter::Init( PdfParser* pParser )
{
    PdfError   eCode;
    PdfVariant cVar;
    const PdfObject* pTrailer;
    PdfReference     ref;

    // clear everything - so that calling Init twice will work
    Clear();

    if( !pParser)
    {
        RAISE_ERROR( ePdfError_InvalidHandle );
    }

    m_pParser      = pParser;
    m_eVersion     = pParser->GetPdfVersion();
    m_vecObjects   = pParser->GetObjects();

    pTrailer = m_pParser->GetTrailer();
    if( pTrailer )
    {
		// load the Catalog/Root object
        SAFE_OP( pTrailer->GetKeyValueVariant( "Root", cVar ) );
        if( cVar.GetDataType() != ePdfDataType_Reference )
        {
            RAISE_ERROR( ePdfError_InvalidDataType );
        }

        ref = cVar.GetReference();
        printf("/Catalog ref=%s\n", ref.ToString().c_str());
        m_pCatalog = m_vecObjects.GetObject( ref );
        if( !m_pCatalog )
        {
            fprintf( stderr, "Error: No catalog dictionary found in the trailer.\n" );
            eCode = ePdfError_InvalidHandle;
        }

		// see if there is an Info dict present - and if so, load it
		SAFE_OP( pTrailer->GetKeyValueVariant( "Info", cVar ) );
		if( cVar.GetDataType() != ePdfDataType_Reference )
		{
			RAISE_ERROR( ePdfError_InvalidDataType );
		}

		ref = cVar.GetReference();
		printf("/Info ref=%s\n", ref.ToString().c_str());
		m_pInfo = m_vecObjects.GetObject( ref );
		// no need to check error, since it's optional
    }

    // clear the parsers object value
    // other PdfWriter and PdfParser
    // would delete the same objects
    pParser->m_vecObjects.clear();

    if( eCode.IsError() )
    {
        RAISE_ERROR( eCode.Error() );
    }

    return eCode;
}

PdfError PdfWriter::Write( const char* pszFilename )
{
    PdfError        eCode;
    PdfOutputDevice device;

    if( !pszFilename )
    {
        RAISE_ERROR( ePdfError_InvalidHandle );
    }

    SAFE_OP( device.Init( pszFilename ) );
    SAFE_OP( this->Write( &device ) );

    return eCode;
}

PdfError PdfWriter::Write( PdfOutputDevice* pDevice )
{
    PdfError eCode;

    // Start with an empty XRefTable
    m_vecXRef.clear();

    if( !pDevice )
    {
        RAISE_ERROR( ePdfError_InvalidHandle );
    }

    SAFE_OP( WritePdfHeader( pDevice ) );
    SAFE_OP( WritePdfObjects( pDevice, m_vecObjects ) );
    SAFE_OP( WritePdfTableOfContents( pDevice ) );

    return eCode;
}

PdfError PdfWriter::WritePdfHeader( PdfOutputDevice* pDevice )
{
    PdfError eCode;
    SAFE_OP( pDevice->Print( "%s\n%%%s", s_szPdfVersions[(int)m_eVersion], PDF_MAGIC ) );
    return eCode;
}

PdfError PdfWriter::CompressObjects( const TVecObjects& vecObjects ) 
{
    PdfError      eCode;
    TCIVecObjects itObjects  = vecObjects.begin();

    while( itObjects != vecObjects.end() )
    {
        // make sure that all objects are FlateDecoded if compression is enabled
        if( m_bCompress )
        {
            SAFE_OP( (*itObjects)->FlateDecodeStream() );
        }

        ++itObjects;
    }

    return eCode;
}

PdfError PdfWriter::WritePdfObjects( PdfOutputDevice* pDevice, const TVecObjects& vecObjects, bool bFillXRefOnly )
{
    PdfError           eCode;
    TCIVecObjects      itObjects  = vecObjects.begin();
    TXRefEntry         tEntry; 
    unsigned int       nLast      = 0;
    TXRefTable         tXRef;
    TXRefTable*        pCur       = NULL;

    tEntry.lGeneration = 0;
    tXRef .nFirst      = 0;
    tXRef .nCount      = 0;

    SAFE_OP( this->CompressObjects( vecObjects ) );

    while( itObjects != vecObjects.end() )
    {
        tEntry.lOffset     = (*itObjects)->IsEmptyEntry() ? 0 : pDevice->Length();
        tEntry.lGeneration = (*itObjects)->IsEmptyEntry() ?  EMPTY_OBJECT_OFFSET : 0;
        tEntry.cUsed       = (*itObjects)->IsEmptyEntry() ? 'f' : 'n';

        if( !pCur || ++nLast != (*itObjects)->ObjectNumber() )
        {
            tXRef.nFirst = (*itObjects)->ObjectNumber();
            tXRef.nCount = 0;
            tXRef.vecOffsets.clear();
            
            m_vecXRef.push_back( tXRef );
            pCur = &(m_vecXRef.back());
        }
        
        pCur->vecOffsets.push_back( tEntry );
        pCur->nCount++;
        
        nLast = (*itObjects)->ObjectNumber();

        if( !bFillXRefOnly )
        {
            SAFE_OP( (*itObjects)->Write( pDevice ) );
        }

        ++itObjects;
    }

    return eCode;
}

PdfError PdfWriter::WriteXRefEntries( PdfOutputDevice* pDevice, const TVecOffsets & vecOffsets )
{
    PdfError          eCode;
    TCIVecOffsets     itOffsets = vecOffsets.begin();

    if( !pDevice )
    {
        RAISE_ERROR( ePdfError_InvalidHandle );
    }

    while( itOffsets != vecOffsets.end() )
    {
        SAFE_OP( pDevice->Print( "%0.10i %0.5i %c \n", (*itOffsets).lOffset, (*itOffsets).lGeneration, (*itOffsets).cUsed ) );
        ++itOffsets;
    }

    return eCode;
}

PdfError PdfWriter::WritePdfTableOfContents( PdfOutputDevice* pDevice )
{
    PdfError          eCode;
    long              lXRef;
    PdfVariant        var;
    std::string       str;
    unsigned int      nSize     = 0;
    TCIVecXRefTable   it;

    lXRef = pDevice->Length();
    SAFE_OP( pDevice->Print( "xref\n" ) );

    it = m_vecXRef.begin();
    while( it != m_vecXRef.end() )
    {
        nSize = ( nSize > (*it).nFirst + (*it).nCount ? nSize : (*it).nFirst + (*it).nCount );

        SAFE_OP( pDevice->Print( "%u %u\n", (*it).nFirst, (*it).nCount ) );
        SAFE_OP( WriteXRefEntries( pDevice, (*it).vecOffsets ) );

        ++it;
    }

    // write the trailer dictionary, see Reference manual: p. 73
    SAFE_OP( pDevice->Print( "trailer\n<<\n/Size %u\n", nSize ) );

    if( m_pParser )
    {
        if( !m_pParser->GetTrailer() )
        {
            RAISE_ERROR( ePdfError_InvalidHandle );
        }

        // Prev is ignored as we write only one crossref section
        SAFE_OP( m_pParser->GetTrailer()->GetKeyValueVariant( "Root", var ) );
        SAFE_OP( var.ToString( str ) );
        SAFE_OP( pDevice->Print( "/Root " ) );
        SAFE_OP( pDevice->Print( str.c_str() ) );
        SAFE_OP( pDevice->Print( "\n" ) );

        if( m_pParser->GetTrailer()->HasKey( "Encrypt" ) )
        {
            SAFE_OP( m_pParser->GetTrailer()->GetKeyValueVariant( "Encrypt", var ) );
            SAFE_OP( var.ToString( str ) );
            SAFE_OP( pDevice->Print( "/Encrypt " ) );
            SAFE_OP( pDevice->Print( str.c_str() ) );
            SAFE_OP( pDevice->Print( "\n" ) );
        }

        if( m_pParser->GetTrailer()->HasKey( "Info" ) )
        {
            SAFE_OP( m_pParser->GetTrailer()->GetKeyValueVariant( "Info", var ) );
            SAFE_OP( var.ToString( str ) );
            SAFE_OP( pDevice->Print( "/Info " ) );
            SAFE_OP( pDevice->Print( str.c_str() ) );
            SAFE_OP( pDevice->Print( "\n" ) );
        }

        if( m_pParser->GetTrailer()->HasKey( "ID" ) )
        {
            SAFE_OP( m_pParser->GetTrailer()->GetKeyValueVariant( "ID", var ) );
            SAFE_OP( var.ToString( str ) );
            SAFE_OP( pDevice->Print( "/ID " ) );
            SAFE_OP( pDevice->Print( str.c_str() ) );
            SAFE_OP( pDevice->Print( "\n" ) );
        }
    }
    else
    {
		// Info dict is optional - so only bother writing if present
        if( m_pInfo )
        {
			SAFE_OP( pDevice->Print( "/Info %s\n", m_pInfo->Reference().ToString().c_str() ) );
        }

		// Catalog, however, is required!
		if( !m_pCatalog )
		{
			RAISE_ERROR( ePdfError_InvalidHandle );
		} else {
			SAFE_OP( pDevice->Print( "/Root %s\n", m_pCatalog->Reference().ToString().c_str() ) );
		}
    }

    SAFE_OP( pDevice->Print( ">>\nstartxref\n%li\n%%%%EOF\n", lXRef ) );

    return eCode;
}

PdfObject* PdfWriter::RemoveObject( const PdfReference & ref )
{
    return m_vecObjects.RemoveObject( ref );
}

PdfError PdfWriter::GetByteOffset( PdfObject* pObject, unsigned long* pulOffset )
{
    PdfError        eCode;
    TVecObjects     vecObj = this->GetObjects();
    TCIVecObjects   it     = vecObj.begin();
    PdfOutputDevice deviceHeader;
    unsigned long   lLen;

    if( !pObject || !pulOffset )
    {
        RAISE_ERROR( ePdfError_InvalidHandle );
    }

    SAFE_OP( deviceHeader.Init() );
    SAFE_OP( this->WritePdfHeader( &deviceHeader ) );

    *pulOffset = deviceHeader.Length();

    while( it != vecObj.end() )
    {
        if( (*it) == pObject )
            break;

        SAFE_OP( (*it)->GetObjectLength( &lLen ) );
        *pulOffset += lLen;

        ++it;
    }
    
    return eCode;
}

PdfError PdfWriter::WriteToBuffer( char** ppBuffer, unsigned long* pulLen )
{
    PdfError        eCode;
    PdfOutputDevice device;

    if( !pulLen )
    {
        RAISE_ERROR( ePdfError_InvalidHandle );
    }

    SAFE_OP( device.Init() );
    SAFE_OP( this->Write( &device ) );

    *pulLen = device.Length();
    *ppBuffer = (char*)malloc( *pulLen * sizeof(char) );
    if( !*ppBuffer )
    {
        RAISE_ERROR( ePdfError_OutOfMemory );
    }

    SAFE_OP( device.Init( *ppBuffer, *pulLen ) );
    SAFE_OP( this->Write( &device ) );
   
    return eCode;
}

};

