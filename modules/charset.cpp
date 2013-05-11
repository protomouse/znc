/*
 * Copyright (C) 2004-2013  See the AUTHORS file for details.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <znc/Modules.h>
#include <unicode/ucsdet.h>
#include <unicode/ucnv.h>

#define CONVERT_BUFFER_LEN UCNV_GET_MAX_BYTES_FOR_STRING(512, 4) // sufficient for the maximum RFC2812 message length in pure Unicode

class CCharsetMod : public CModule
{
private:
	VCString m_vsClientCharsets;
	VCString m_vsServerCharsets;
	bool m_bGuess; // try to guess source charsets before applying specified ones
	bool m_bOnlyServer; // only convert messages from server to client

	UCharsetDetector *m_pCharsetDetector;

	bool GuessCharsets(const CString& sData, VCString &charsets)
	{
		UErrorCode uErr = U_ZERO_ERROR;
		ucsdet_setText(m_pCharsetDetector, sData.data(), sData.size(), &uErr);

		if(U_SUCCESS(uErr))
		{
			uErr = U_ZERO_ERROR;
			int nMatches;
			const UCharsetMatch **matches = ucsdet_detectAll(m_pCharsetDetector, &nMatches, &uErr);

			if(U_SUCCESS(uErr))
			{
				uErr = U_ZERO_ERROR;
				int i;

				for(i = 0; i < nMatches; ++i)
				{
					charsets.insert(charsets.end(), ucsdet_getName(matches[i], &uErr));
				}

				return true;
			}
		}

		return false;
	}

	bool ConvertCharset(const CString& sFrom, const CString& sTo, CString& sData)
	{
		int nBytes;
		UErrorCode uErr = U_ZERO_ERROR;
		char buf[CONVERT_BUFFER_LEN] = {0};

		nBytes = ucnv_convert(sTo.c_str(), sFrom.c_str(), buf, CONVERT_BUFFER_LEN, sData.data(), sData.size(), &uErr);

		if(U_FAILURE(uErr) && uErr != U_BUFFER_OVERFLOW_ERROR)
		{
			return false;
		}
		else
		{
			sData.assign(buf, nBytes);
			return true;
		}
	}

	bool ConvertCharset(const VCString& vsFrom, const CString& sTo, CString& sData)
	{
		CString sDataCopy(sData);

		bool bConverted = false;
		VCString srcCharsets;

		if(m_bGuess)
		{
			GuessCharsets(sData, srcCharsets);
		}

		srcCharsets.insert(srcCharsets.end(), vsFrom.begin(), vsFrom.end());

		// try all possible source charsets:
		for(VCString::const_iterator itf = srcCharsets.begin(); itf != srcCharsets.end(); ++itf)
		{
			if(ConvertCharset(*itf, sTo, sDataCopy))
			{
				// conversion successful!
				sData = sDataCopy;
				bConverted = true;
				break;
			}
			else
			{
				// reset string and try the next charset:
				sDataCopy = sData;
			}
		}

		return bConverted;
	}

	bool OpenCharsetDetector(void)
	{
		UErrorCode uErr = U_ZERO_ERROR;
		m_pCharsetDetector = ucsdet_open(&uErr);

		return U_SUCCESS(uErr);
	}

	void CloseCharsetDetector(void)
	{
		if (m_pCharsetDetector)
		{
			ucsdet_close(m_pCharsetDetector);
			m_pCharsetDetector = NULL;
		}
	}

	bool CanConvertToUnicode(const CString& sCharset)
	{
		UConverter *conv;
		UErrorCode uErr = U_ZERO_ERROR;

		conv = ucnv_open(sCharset.c_str(), &uErr);

		if(U_SUCCESS(uErr))
		{
			ucnv_close(conv);
			return true;
		}
		else
		{
			return false;
		}
	}

public:
	MODCONSTRUCTOR(CCharsetMod)
	{
		m_bGuess = false;
		m_bOnlyServer = false;
		m_pCharsetDetector = NULL;
	}

	virtual ~CCharsetMod()
	{
		CloseCharsetDetector();
	}

	bool OnLoad(const CString& sArgs, CString& sMessage)
	{
		size_t uIndex = 0;

		if(sArgs.Token(uIndex).Equals("-guess"))
		{
			if(!OpenCharsetDetector())
			{
				sMessage = "Could not open charset detector.";
				return false;
			}

			m_bGuess = true;
			++uIndex;
		}

		if(sArgs.Token(uIndex).Equals("-onlyserver"))
		{
			m_bOnlyServer = true;
			++uIndex;
		}

		if(sArgs.Token(uIndex + 1).empty() || !sArgs.Token(uIndex + 2).empty())
		{
			sMessage = "This module needs two charset lists as arguments: [-guess] [-onlyserver] "
				"<client_charset1[,client_charset2[,...]]> "
				"<server_charset1[,server_charset2[,...]]>";
			return false;
			// the first charset in each list is the preferred one for
			// messages to the client / to the server.
		}

		VCString vsFrom, vsTo;
		sArgs.Token(uIndex).Split(",", vsFrom);
		sArgs.Token(uIndex + 1).Split(",", vsTo);

		// probe conversions:
		for(VCString::const_iterator itf = vsFrom.begin(); itf != vsFrom.end(); ++itf)
		{
			for(VCString::const_iterator itt = vsTo.begin(); itt != vsTo.end(); ++itt)
			{
				if(!CanConvertToUnicode(*itf))
				{
					sMessage = "Cannot convert '" + *itf + "'.";
					return false;
				}

				if(!CanConvertToUnicode(*itt))
				{
					sMessage = "Cannot convert '" + *itt + "'.";
					return false;
				}
			}
		}

		m_vsClientCharsets = vsFrom;
		m_vsServerCharsets = vsTo;

		return true;
	}

	EModRet OnRaw(CString& sLine)
	{
		// convert IRC server -> client
		ConvertCharset(m_vsServerCharsets, m_vsClientCharsets[0], sLine);
		return CONTINUE;
	}

	EModRet OnUserRaw(CString& sLine)
	{
		// convert client -> IRC server
		if(!m_bOnlyServer)
		{
			ConvertCharset(m_vsClientCharsets, m_vsServerCharsets[0], sLine);
		}

		return CONTINUE;
	}

};

template<> void TModInfo<CCharsetMod>(CModInfo& Info)
{
	Info.SetWikiPage("charset");
	Info.SetHasArgs(true);
	Info.SetArgsHelpText("Two charset lists: [-guess] [-onlyserver] "
						 "<client_charset1[,client_charset2[,...]]> "
						 "<server_charset1[,server_charset2[,...]]>");
}

USERMODULEDEFS(CCharsetMod, "Normalizes character encodings.")
