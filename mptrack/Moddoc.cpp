// moddoc.cpp : implementation of the CModDoc class
//

#include "stdafx.h"
#include "mptrack.h"
#include "mainfrm.h"
#include "moddoc.h"
#include "childfrm.h"
#include "mpdlgs.h"
#include "dlg_misc.h"
#include "dlsbank.h"
#include "mod2wave.h"
#include "mod2midi.h"
#include "vstplug.h"


#pragma warning(disable:4244)

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif


/////////////////////////////////////////////////////////////////////////////
// CModDoc

IMPLEMENT_SERIAL(CModDoc, CDocument, 0 /* schema number*/ )

BEGIN_MESSAGE_MAP(CModDoc, CDocument)
	//{{AFX_MSG_MAP(CModDoc)
	ON_COMMAND(ID_FILE_SAVEASWAVE,		OnFileWaveConvert)
	ON_COMMAND(ID_FILE_SAVEASMP3,		OnFileMP3Convert)
	ON_COMMAND(ID_FILE_SAVEMIDI,		OnFileMidiConvert)
	ON_COMMAND(ID_PLAYER_PLAY,			OnPlayerPlay)
	ON_COMMAND(ID_PLAYER_PAUSE,			OnPlayerPause)
	ON_COMMAND(ID_PLAYER_STOP,			OnPlayerStop)
	ON_COMMAND(ID_PLAYER_PLAYFROMSTART,	OnPlayerPlayFromStart)
	ON_COMMAND(ID_VIEW_GLOBALS,			OnEditGlobals)
	ON_COMMAND(ID_VIEW_PATTERNS,		OnEditPatterns)
	ON_COMMAND(ID_VIEW_SAMPLES,			OnEditSamples)
	ON_COMMAND(ID_VIEW_INSTRUMENTS,		OnEditInstruments)
	ON_COMMAND(ID_VIEW_COMMENTS,		OnEditComments)
	ON_COMMAND(ID_INSERT_PATTERN,		OnInsertPattern)
	ON_COMMAND(ID_INSERT_SAMPLE,		OnInsertSample)
	ON_COMMAND(ID_INSERT_INSTRUMENT,	OnInsertInstrument)
	ON_COMMAND(ID_CLEANUP_SAMPLES,		OnCleanupSamples)
	ON_COMMAND(ID_CLEANUP_INSTRUMENTS,	OnCleanupInstruments)
	ON_COMMAND(ID_CLEANUP_PATTERNS,		OnCleanupPatterns)
	ON_COMMAND(ID_CLEANUP_SONG,			OnCleanupSong)
	ON_COMMAND(ID_CLEANUP_REARRANGE,	OnRearrangePatterns)
	ON_COMMAND(ID_INSTRUMENTS_REMOVEALL,OnRemoveAllInstruments)
	ON_COMMAND(ID_ESTIMATESONGLENGTH,	OnEstimateSongLength)
	ON_COMMAND(ID_PATTERN_PLAY,			OnPatternPlay)				//rewbs.patPlayAllViews
	ON_COMMAND(ID_PATTERN_PLAYNOLOOP,	OnPatternPlayNoLoop)		//rewbs.patPlayAllViews
	ON_COMMAND(ID_PATTERN_RESTART,		OnPatternRestart)		//rewbs.patPlayAllViews
	ON_UPDATE_COMMAND_UI(ID_INSERT_INSTRUMENT,		OnUpdateXMITOnly)
	ON_UPDATE_COMMAND_UI(ID_INSTRUMENTS_REMOVEALL,	OnUpdateInstrumentOnly)
	ON_UPDATE_COMMAND_UI(ID_CLEANUP_INSTRUMENTS,	OnUpdateInstrumentOnly)
	ON_UPDATE_COMMAND_UI(ID_VIEW_INSTRUMENTS,		OnUpdateXMITOnly)
	ON_UPDATE_COMMAND_UI(ID_VIEW_COMMENTS,			OnUpdateXMITOnly)
	ON_UPDATE_COMMAND_UI(ID_FILE_SAVEASMP3,			OnUpdateMP3Encode)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()


////////////////////////////////////////////////////////////////////////////
// CModDoc diagnostics

#ifdef _DEBUG
void CModDoc::AssertValid() const
{
	CDocument::AssertValid();
}

void CModDoc::Dump(CDumpContext& dc) const
{
	CDocument::Dump(dc);
}
#endif //_DEBUG


/////////////////////////////////////////////////////////////////////////////
// CModDoc construction/destruction

CModDoc::CModDoc()
//----------------
{
	m_bPaused = TRUE;
	m_lpszLog = NULL;
	m_hWndFollow = NULL;
	memset(PatternUndo, 0, sizeof(PatternUndo));
	memset(OrderUndo, 0, sizeof(OrderUndo));
#ifdef _DEBUG
	MODCHANNEL *p = m_SndFile.Chn;
	if (((DWORD)p) & 7) Log("MODCHANNEL is not aligned (0x%08X)\n", p);
#endif
}


CModDoc::~CModDoc()
//-----------------
{
	ClearUndo();
	ClearLog();
}


void CModDoc::SetModifiedFlag(BOOL bModified)
//-------------------------------------------
{
	BOOL bChanged = (bModified != IsModified());
	CDocument::SetModifiedFlag(bModified);
	if (bChanged) UpdateFrameCounts();
}


BOOL CModDoc::OnNewDocument()
//---------------------------
{
	if (!CDocument::OnNewDocument()) return FALSE;
	m_SndFile.Create(NULL, 0);
	m_SndFile.m_nType = CTrackApp::GetDefaultDocType();
	if (m_SndFile.m_nType & (MOD_TYPE_XM|MOD_TYPE_IT)) m_SndFile.m_dwSongFlags |= SONG_LINEARSLIDES;
	theApp.GetDefaultMidiMacro(&m_SndFile.m_MidiCfg);
	InitializeMod();
	SetModifiedFlag(FALSE);
	return TRUE;
}


BOOL CModDoc::OnOpenDocument(LPCTSTR lpszPathName)
//------------------------------------------------
{
	BOOL bModified = TRUE;
	CMappedFile f;

	if (!lpszPathName) return OnNewDocument();
	BeginWaitCursor();
	if (f.Open(lpszPathName))
	{
		DWORD dwLen = f.GetLength();
		if (dwLen)
		{
			LPBYTE lpStream = f.Lock();
			if (lpStream)
			{
				m_SndFile.Create(lpStream, dwLen);
				f.Unlock();
			}
		}
		f.Close();
	}
	EndWaitCursor();
	if ((m_SndFile.m_nType == MOD_TYPE_NONE) || (!m_SndFile.m_nChannels)) return FALSE;
	// Midi Import
	if (m_SndFile.m_nType == MOD_TYPE_MID)
	{
		CDLSBank *pCachedBank = NULL, *pEmbeddedBank = NULL;
		CHAR szCachedBankFile[_MAX_PATH] = "";

		if (CDLSBank::IsDLSBank(lpszPathName))
		{
			pEmbeddedBank = new CDLSBank();
			pEmbeddedBank->Open(lpszPathName);
		}
		m_SndFile.m_nType = MOD_TYPE_IT;
		BeginWaitCursor();
		LPMIDILIBSTRUCT lpMidiLib = CTrackApp::GetMidiLibrary();
		// Scan Instruments
		if (lpMidiLib) for (UINT nIns=1; nIns<=m_SndFile.m_nInstruments; nIns++) if (m_SndFile.Headers[nIns])
		{
			LPCSTR pszMidiMapName;
			INSTRUMENTHEADER *penv = m_SndFile.Headers[nIns];
			UINT nMidiCode;
			BOOL bEmbedded = FALSE;

			if (penv->nMidiChannel == 10)
				nMidiCode = 0x80 | (penv->nMidiDrumKey & 0x7F);
			else
				nMidiCode = penv->nMidiProgram & 0x7F;
			pszMidiMapName = lpMidiLib->MidiMap[nMidiCode];
			if (pEmbeddedBank)
			{
				UINT nDlsIns = 0, nDrumRgn = 0;
				UINT nProgram = penv->nMidiProgram;
				UINT dwKey = (nMidiCode < 128) ? 0xFF : (nMidiCode & 0x7F);
				if ((pEmbeddedBank->FindInstrument(	(nMidiCode >= 128),
													(penv->wMidiBank & 0x3FFF),
													nProgram, dwKey, &nDlsIns))
				 || (pEmbeddedBank->FindInstrument(	(nMidiCode >= 128),	0xFFFF,
													(nMidiCode >= 128) ? 0xFF : nProgram,
													dwKey, &nDlsIns)))
				{
					if (dwKey < 0x80) nDrumRgn = pEmbeddedBank->GetRegionFromKey(nDlsIns, dwKey);
					if (pEmbeddedBank->ExtractInstrument(&m_SndFile, nIns, nDlsIns, nDrumRgn))
					{
						if ((dwKey >= 24) && (dwKey < 100))
						{
							lstrcpyn(penv->name, szMidiPercussionNames[dwKey-24], sizeof(penv->name));
						}
						bEmbedded = TRUE;
					}
				}
			}
			if ((pszMidiMapName) && (pszMidiMapName[0]) && (!bEmbedded))
			{
				// Load From DLS Bank
				if (CDLSBank::IsDLSBank(pszMidiMapName))
				{
					CDLSBank *pDLSBank = NULL;
					
					if ((pCachedBank) && (!lstrcmpi(szCachedBankFile, pszMidiMapName)))
					{
						pDLSBank = pCachedBank;
					} else
					{
						if (pCachedBank) delete pCachedBank;
						pCachedBank = new CDLSBank;
						strcpy(szCachedBankFile, pszMidiMapName);
						if (pCachedBank->Open(pszMidiMapName)) pDLSBank = pCachedBank;
					}
					if (pDLSBank)
					{
						UINT nDlsIns = 0, nDrumRgn = 0;
						UINT nProgram = penv->nMidiProgram;
						UINT dwKey = (nMidiCode < 128) ? 0xFF : (nMidiCode & 0x7F);
						if ((pDLSBank->FindInstrument(	(nMidiCode >= 128),
														(penv->wMidiBank & 0x3FFF),
														nProgram, dwKey, &nDlsIns))
						 || (pDLSBank->FindInstrument(	(nMidiCode >= 128), 0xFFFF,
														(nMidiCode >= 128) ? 0xFF : nProgram,
														dwKey, &nDlsIns)))
						{
							if (dwKey < 0x80) nDrumRgn = pDLSBank->GetRegionFromKey(nDlsIns, dwKey);
							pDLSBank->ExtractInstrument(&m_SndFile, nIns, nDlsIns, nDrumRgn);
							if ((dwKey >= 24) && (dwKey < 24+61))
							{
								lstrcpyn(penv->name, szMidiPercussionNames[dwKey-24], sizeof(penv->name));
							}
						}
					}
				} else
				{
					// Load from Instrument or Sample file
					CHAR szName[_MAX_FNAME], szExt[_MAX_EXT];
					CFile f;

					if (f.Open(pszMidiMapName, CFile::modeRead))
					{
						DWORD len = f.GetLength();
						LPBYTE lpFile;
						if ((len) && ((lpFile = (LPBYTE)GlobalAllocPtr(GHND, len)) != NULL))
						{
							f.Read(lpFile, len);
							m_SndFile.ReadInstrumentFromFile(nIns, lpFile, len);
							_splitpath(pszMidiMapName, NULL, NULL, szName, szExt);
							strncat(szName, szExt, sizeof(szName));
							penv = m_SndFile.Headers[nIns];
							if (!penv->filename[0]) lstrcpyn(penv->filename, szName, sizeof(penv->filename));
							if (!penv->name[0])
							{
								if (nMidiCode < 128)
								{
									lstrcpyn(penv->name, szMidiProgramNames[nMidiCode], sizeof(penv->name));
								} else
								{
									UINT nKey = nMidiCode & 0x7F;
									if (nKey >= 24)
										lstrcpyn(penv->name, szMidiPercussionNames[nKey-24], sizeof(penv->name));
								}
							}
						}
						f.Close();
					}
				}
			}
		}
		if (pCachedBank) delete pCachedBank;
		if (pEmbeddedBank) delete pEmbeddedBank;
		EndWaitCursor();
	}
	// Convert to MOD/S3M/XM/IT
	switch(m_SndFile.m_nType)
	{
	case MOD_TYPE_MOD:
	case MOD_TYPE_S3M:
	case MOD_TYPE_XM:
	case MOD_TYPE_IT:
		bModified = FALSE;
		break;
	case MOD_TYPE_AMF0:
	case MOD_TYPE_MTM:
	case MOD_TYPE_669:
		m_SndFile.m_nType = MOD_TYPE_MOD;
		break;
	case MOD_TYPE_MED:
	case MOD_TYPE_OKT:
	case MOD_TYPE_AMS:
	case MOD_TYPE_MT2:
		m_SndFile.m_nType = MOD_TYPE_XM;
		if ((m_SndFile.m_nDefaultTempo == 125) && (m_SndFile.m_nDefaultSpeed == 6) && (!m_SndFile.m_nInstruments))
		{
			m_SndFile.m_nType = MOD_TYPE_MOD;
			for (UINT i=0; i<MAX_PATTERNS; i++)
				if ((m_SndFile.Patterns[i]) && (m_SndFile.PatternSize[i] != 64))
					m_SndFile.m_nType = MOD_TYPE_XM;
		}
		break;
	case MOD_TYPE_FAR:
	case MOD_TYPE_PTM:
	case MOD_TYPE_STM:
	case MOD_TYPE_DSM:
	case MOD_TYPE_AMF:
	case MOD_TYPE_PSM:
		m_SndFile.m_nType = MOD_TYPE_S3M;
		break;
	default:
		m_SndFile.m_nType = MOD_TYPE_IT;
	}
	SetModifiedFlag(FALSE); // (bModified);
	return TRUE;
}


BOOL CModDoc::OnSaveDocument(LPCTSTR lpszPathName)
//------------------------------------------------
{
	static int greccount = 0;
	CHAR fext[_MAX_EXT]="";
	UINT nType = m_SndFile.m_nType, dwPacking = 0;
	BOOL bOk = FALSE;

	if (!lpszPathName) return FALSE;
	_splitpath(lpszPathName, NULL, NULL, NULL, fext);
	if (!lstrcmpi(fext, ".mod")) nType = MOD_TYPE_MOD; else
	if (!lstrcmpi(fext, ".s3m")) nType = MOD_TYPE_S3M; else
	if (!lstrcmpi(fext, ".xm")) nType = MOD_TYPE_XM; else
	if (!lstrcmpi(fext, ".it")) nType = MOD_TYPE_IT; else
	if (!greccount)
	{
		greccount++;
		bOk = DoSave(NULL, TRUE);
		greccount--;
		return bOk;
	}
	BeginWaitCursor();
	switch(nType)
	{
	case MOD_TYPE_MOD:	bOk = m_SndFile.SaveMod(lpszPathName, dwPacking); break;
	case MOD_TYPE_S3M:	bOk = m_SndFile.SaveS3M(lpszPathName, dwPacking); break;
	case MOD_TYPE_XM:	bOk = m_SndFile.SaveXM(lpszPathName, dwPacking); break;
	case MOD_TYPE_IT:	bOk = m_SndFile.SaveIT(lpszPathName, dwPacking); break;
	}
	EndWaitCursor();
	if (bOk)
	{
		if (nType == m_SndFile.m_nType) SetPathName(lpszPathName);
	} else
	{
		ErrorBox(IDS_ERR_SAVESONG, CMainFrame::GetMainFrame());
	}
	return bOk;
}


void CModDoc::OnCloseDocument()
//-----------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm) pMainFrm->OnDocumentClosed(this);
	CDocument::OnCloseDocument();


}


void CModDoc::DeleteContents()
//----------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm) pMainFrm->StopMod(this);
	m_SndFile.Destroy();
}


BOOL CModDoc::DoSave(LPCSTR lpszPathName, BOOL)
//---------------------------------------------
{
	CHAR s[_MAX_PATH] = "";
	CHAR path[_MAX_PATH]="", drive[_MAX_DRIVE]="";
	CHAR fname[_MAX_FNAME]="", fext[_MAX_EXT]="";
	LPCSTR lpszFilter = NULL, lpszDefExt = NULL;
	
	switch(m_SndFile.GetType())
	{
	case MOD_TYPE_MOD:
		lpszDefExt = "mod";
		lpszFilter = "ProTracker Modules (*.mod)|*.mod||";
		strcpy(fext, ".mod");
		break;
	case MOD_TYPE_S3M:
		lpszDefExt = "s3m";
		lpszFilter = "ScreamTracker Modules (*.s3m)|*.s3m||";
		strcpy(fext, ".s3m");
		break;
	case MOD_TYPE_XM:
		lpszDefExt = "xm";
		lpszFilter = "FastTracker Modules (*.xm)|*.xm||";
		strcpy(fext, ".xm");
		break;
	case MOD_TYPE_IT:
		lpszDefExt = "it";
		lpszFilter = "Impulse Tracker Modules (*.it)|*.it||";
		strcpy(fext, ".it");
		break;
	default:	
		ErrorBox(IDS_ERR_SAVESONG, CMainFrame::GetMainFrame());
		return FALSE;
	}
	if ((!lpszPathName) || (!lpszPathName[0]))
	{
		_splitpath(m_strPathName, drive, path, fname, NULL);
		if (!fname[0]) strcpy(fname, m_strTitle);
		strcpy(s, drive);
		strcat(s, path);
		strcat(s, fname);
		strcat(s, fext);
		CFileDialog dlg(FALSE, lpszDefExt, s,
			OFN_HIDEREADONLY| OFN_ENABLESIZING | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN,
			lpszFilter,
			theApp.m_pMainWnd);
		if (CMainFrame::m_szCurModDir[0])
		{
			dlg.m_ofn.lpstrInitialDir = CMainFrame::m_szCurModDir;
		}
		if (dlg.DoModal() != IDOK) return FALSE;
		strcpy(s, dlg.GetPathName());
		_splitpath(s, drive, path, fname, fext);
		strcpy(CMainFrame::m_szCurModDir, drive);
		strcat(CMainFrame::m_szCurModDir, path);
	} else
	{
		_splitpath(lpszPathName, drive, path, fname, NULL);
		strcpy(s, drive);
		strcat(s, path);
		strcat(s, fname);
		strcat(s, fext);
	}
	// Do we need to create a backup file ?
	if ((CMainFrame::m_dwPatternSetup & PATTERN_CREATEBACKUP)
	 && (IsModified()) && (!lstrcmpi(s, m_strPathName)))
	{
		CFileStatus rStatus;
		if (CFile::GetStatus(s, rStatus))
		{
			CHAR bkname[_MAX_PATH] = "";
			_splitpath(s, bkname, path, fname, NULL);
			strcat(bkname, path);
			strcat(bkname, fname);
			strcat(bkname, ".bak");
			if (CFile::GetStatus(bkname, rStatus))
			{
				DeleteFile(bkname);
			}
			MoveFile(s, bkname);
		}
	}
	if (OnSaveDocument(s))
	{
		SetModified(FALSE);
		return TRUE;
	} else
	{
		ErrorBox(IDS_ERR_SAVESONG, CMainFrame::GetMainFrame());
		return FALSE;
	}
}


void CModDoc::InitPlayer()
//------------------------
{
	m_SndFile.ResetChannels();
}


BOOL CModDoc::InitializeMod()
//---------------------------
{
	// New module ?
	if (!m_SndFile.m_nChannels)
	{
		m_SndFile.m_nChannels = (m_SndFile.m_nType & MOD_TYPE_MOD) ? 8 : 16;
		if (m_SndFile.Order[0] >= MAX_PATTERNS)	m_SndFile.Order[0] = 0;
		if (!m_SndFile.Patterns[0])
		{
			m_SndFile.PatternSize[0] = 64;
			m_SndFile.Patterns[0] = CSoundFile::AllocatePattern(m_SndFile.PatternSize[0], m_SndFile.m_nChannels);
		}
		strcpy(m_SndFile.m_szNames[0], "untitled");
		m_SndFile.m_nMusicTempo = m_SndFile.m_nDefaultTempo = 125;
		m_SndFile.m_nMusicSpeed = m_SndFile.m_nDefaultSpeed = 6;
		m_SndFile.m_nGlobalVolume = m_SndFile.m_nDefaultGlobalVolume = 256;
		for (UINT init=0; init<MAX_BASECHANNELS; init++)
		{
			m_SndFile.ChnSettings[init].dwFlags = 0;
			m_SndFile.ChnSettings[init].nVolume = 64;
			if (m_SndFile.m_nType & (MOD_TYPE_XM|MOD_TYPE_IT))
				m_SndFile.ChnSettings[init].nPan = 128;
			else
				m_SndFile.ChnSettings[init].nPan = (init & 0x01) ? 64 : 192;
			m_SndFile.Chn[init].nGlobalVol = 64;
		}
	}
	if (!m_SndFile.m_nSamples)
	{
		strcpy(m_SndFile.m_szNames[1], "untitled");
		m_SndFile.m_nSamples = 1;
		m_SndFile.Ins[1].nVolume = 256;
		m_SndFile.Ins[1].nGlobalVol = 64;
		m_SndFile.Ins[1].nPan = 128;
		m_SndFile.Ins[1].nC4Speed = 8363;
		if ((!m_SndFile.m_nInstruments) && (m_SndFile.m_nType & MOD_TYPE_XM))
		{
			m_SndFile.m_nInstruments = 1;
			m_SndFile.Headers[1] = new INSTRUMENTHEADER;
			InitializeInstrument(m_SndFile.Headers[1], 1);
		}
		if (m_SndFile.m_nType & (MOD_TYPE_IT|MOD_TYPE_XM))
		{
			m_SndFile.m_dwSongFlags |= SONG_LINEARSLIDES;
		}
	}
	m_SndFile.SetCurrentPos(0);
	return TRUE;
}


void CModDoc::PostMessageToAllViews(UINT uMsg, WPARAM wParam, LPARAM lParam)
//--------------------------------------------------------------------------
{
	POSITION pos = GetFirstViewPosition();
	while (pos != NULL)
	{
		CView* pView = GetNextView(pos);
		if (pView) pView->PostMessage(uMsg, wParam, lParam);
	} 
}


void CModDoc::SendMessageToActiveViews(UINT uMsg, WPARAM wParam, LPARAM lParam)
//-----------------------------------------------------------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm)
	{
		CMDIChildWnd *pMDI = pMainFrm->MDIGetActive();
		if (pMDI) pMDI->SendMessageToDescendants(uMsg, wParam, lParam);
	}
}


void CModDoc::ViewPattern(UINT nPat, UINT nOrd)
//---------------------------------------------
{
	SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW, IDD_CONTROL_PATTERNS, ((nPat+1) << 16) | nOrd);
}


void CModDoc::ViewSample(UINT nSmp)
//---------------------------------
{
	SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW, IDD_CONTROL_SAMPLES, nSmp);
}


void CModDoc::ViewInstrument(UINT nIns)
//-------------------------------------
{
	SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW, IDD_CONTROL_INSTRUMENTS, nIns);
}


BOOL CModDoc::AddToLog(LPCSTR lpszLog)
//------------------------------------
{
	UINT len;
	if ((!lpszLog) || (!lpszLog[0])) return FALSE;
	len = strlen(lpszLog);
	if (m_lpszLog)
	{
		LPSTR p = new CHAR[strlen(m_lpszLog)+len+2];
		if (p)
		{
			strcpy(p, m_lpszLog);
			strcat(p, lpszLog);
			delete m_lpszLog;
			m_lpszLog = p;
		}
	} else
	{
		m_lpszLog = new CHAR[len+2];
		if (m_lpszLog) strcpy(m_lpszLog, lpszLog);
	}
	return TRUE;
}


BOOL CModDoc::ClearLog()
//----------------------
{
	if (m_lpszLog)
	{
		delete m_lpszLog;
		m_lpszLog = NULL;
	}
	return TRUE;
}


UINT CModDoc::ShowLog(LPCSTR lpszTitle, CWnd *parent)
//---------------------------------------------------
{
	if (!lpszTitle) lpszTitle = MAINFRAME_TITLE;
	if (m_lpszLog)
	{
#if 0
		CShowLogDlg dlg(parent);
		return dlg.ShowLog(m_lpszLog, lpszTitle);
#else
		return ::MessageBox((parent) ? parent->m_hWnd : NULL, m_lpszLog, lpszTitle, MB_OK|MB_ICONINFORMATION);
#endif
	}
	return IDCANCEL;
}

//static gdwLastMixActiveTime = 0;
UINT CModDoc::PlayNote(UINT note, UINT nins, UINT nsmp, BOOL bpause, LONG nVol, LONG loopstart, LONG loopend, UINT nCurrentChn) //rewbs.vstiLive: added current chan param
//-----------------------------------------------------------------------------------------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	UINT nChn = m_SndFile.m_nChannels;
	
	if ((!pMainFrm) || (!note)) return FALSE;
	if (nVol > 256) nVol = 256;
	if (note < 128)
	{
		BEGIN_CRITICAL();
		if ((bpause) || (m_SndFile.IsPaused()))
		{
			pMainFrm->SetLastMixActiveTime();
			BOOL bFound = FALSE;
			// All notes off
			for (UINT i=0; i<MAX_CHANNELS; i++)
			{
				if ((i < m_SndFile.m_nChannels) || (m_SndFile.Chn[i].nMasterChn))
				{
					m_SndFile.Chn[i].dwFlags |= CHN_KEYOFF | CHN_NOTEFADE;
					m_SndFile.Chn[i].nFadeOutVol = 0;
				}
			}
			// Search for available channel
			for (UINT j=m_SndFile.m_nChannels; j<MAX_CHANNELS; j++)
			{
				MODCHANNEL *p = &m_SndFile.Chn[j];
				if (!p->nLength)
				{
					bFound = TRUE;
					nChn = j;
					break;
				}
			}
			if (!bFound)
			{
				// Not found: look for one that's stopped
				for (UINT j=m_SndFile.m_nChannels; j<MAX_CHANNELS; j++)
				{
					MODCHANNEL *p = &m_SndFile.Chn[j];
					if (p->dwFlags & CHN_NOTEFADE)
					{
						bFound = TRUE;
						nChn = j;
						break;
					}
				}
			}
		}
		MODCHANNEL *pChn = &m_SndFile.Chn[nChn];
		if (pChn->nLength)
		{
			pChn->nPos = pChn->nPosLo = pChn->nLength = 0;
		}
		pChn->dwFlags &= 0xFF;
		pChn->dwFlags &= ~(CHN_MUTE);
		pChn->nGlobalVol = 64;
		pChn->nInsVol = 64;
		pChn->nPan = 128;
		pChn->nNewNote = note;
		pChn->nRightVol = pChn->nLeftVol = 0;
		pChn->nROfs = pChn->nLOfs = 0;
		pChn->nCutOff = 0x7F;
		pChn->nResonance = 0;
		if (nins)
		{
			pChn->nVolEnvPosition = 0;
			pChn->nPanEnvPosition = 0;
			pChn->nPitchEnvPosition = 0;
			m_SndFile.InstrumentChange(pChn, nins);
		} else
		if ((nsmp) && (nsmp < MAX_SAMPLES))
		{
			MODINSTRUMENT *pins = &m_SndFile.Ins[nsmp];
			pChn->pCurrentSample = pins->pSample;
			pChn->pHeader = NULL;
			pChn->pInstrument = pins;
			pChn->pSample = pins->pSample;
			pChn->nFineTune = pins->nFineTune;
			pChn->nC4Speed = pins->nC4Speed;
			pChn->nPos = pChn->nPosLo = pChn->nLength = 0;
			pChn->nLoopStart = pins->nLoopStart;
			pChn->nLoopEnd = pins->nLoopEnd;
			pChn->dwFlags = pins->uFlags & (0xFF & ~CHN_MUTE);
			pChn->nPan = 128;
			if (pins->uFlags & CHN_PANNING) pChn->nPan = pins->nPan;
			pChn->nInsVol = pins->nGlobalVol;
			pChn->nFadeOutVol = 0x10000;
		}
		pChn->nVolume = 256;
		m_SndFile.NoteChange(nChn, note, FALSE, TRUE, TRUE);
		if (nVol > 0) pChn->nVolume = nVol;
		pChn->nMasterChn = 0;
		if (bpause)
		{   
			if ((loopstart + 16 < loopend) && (loopstart >= 0) && (loopend <= (LONG)pChn->nLength))
			{
				pChn->nPos = loopstart;
				pChn->nPosLo = 0;
				pChn->nLoopStart = loopstart;
				pChn->nLoopEnd = loopend;
				pChn->nLength = loopend;
			}
			m_SndFile.m_nBufferCount = 0;
			m_SndFile.m_dwSongFlags |= SONG_PAUSED;
			if ((!(CMainFrame::m_dwPatternSetup & PATTERN_NOEXTRALOUD)) && (nsmp)) pChn->dwFlags |= CHN_EXTRALOUD;
		} else pChn->dwFlags &= ~CHN_EXTRALOUD;
		END_CRITICAL();
		//rewbs.vstiLive
		if (nins <= m_SndFile.m_nInstruments)
		{
			INSTRUMENTHEADER *penv = m_SndFile.Headers[nins];
			//MODINSTRUMENT *psmp = &(m_SndFile.Ins[nins]);
			if (nCurrentChn >=0 && penv && penv->nMidiChannel > 0 && penv->nMidiChannel < 17) // instro sends to a midi chan
			{
				UINT nPlugin = 0;
				if (pChn->pHeader) 
					nPlugin = pChn->pHeader->nMixPlug;  					// first try intrument VST
				if ((!nPlugin) || (nPlugin > MAX_MIXPLUGINS))
					nPlugin = m_SndFile.ChnSettings[nCurrentChn+1].nMixPlugin; // Then try Channel VST                    
   				if ((nPlugin) && (nPlugin <= MAX_MIXPLUGINS))
				{
					IMixPlugin *pPlugin =  m_SndFile.m_MixPlugins[nPlugin-1].pMixPlugin;
					if (pPlugin) pPlugin->MidiCommand(penv->nMidiChannel, penv->nMidiProgram, note, nVol ? nVol : 64, nCurrentChn);
				}
			}
		}
		//end rewbs.vstiLive
		if (pMainFrm->GetModPlaying() != this)
		{
			m_SndFile.m_dwSongFlags |= SONG_PAUSED;
			pMainFrm->PlayMod(this, m_hWndFollow, m_dwNotifyType);
		}
		CMainFrame::EnableLowLatencyMode();
	} else
	{
		BEGIN_CRITICAL();
		m_SndFile.NoteChange(nChn, note);
		if (bpause) m_SndFile.m_dwSongFlags |= SONG_PAUSED;
		END_CRITICAL();
	}
	return nChn;
}


BOOL CModDoc::NoteOff(UINT note, BOOL bFade, UINT nins, UINT nCurrentChn) //rewbs.vstiLive: added chan and nins
//--------------------------------------------------------------------------
{
	BEGIN_CRITICAL();
	MODCHANNEL *pChn = &m_SndFile.Chn[m_SndFile.m_nChannels];
	for (UINT i=m_SndFile.m_nChannels; i<MAX_CHANNELS; i++, pChn++) if (!pChn->nMasterChn)
	{
		DWORD mask = (bFade) ? CHN_NOTEFADE : (CHN_NOTEFADE|CHN_KEYOFF);

		//rewbs.vstiLive
		if (nins>0 && nins<=m_SndFile.m_nInstruments)
		{
			INSTRUMENTHEADER *penv = m_SndFile.Headers[nins];
//			MODINSTRUMENT *psmp = &(m_SndFile.Ins[nins]);
			if (nCurrentChn >=0 && penv && penv->nMidiChannel > 0 && penv->nMidiChannel < 17) // instro sends to a midi chan
			{
				UINT nPlugin = 0;
				if (pChn->pHeader) 
					nPlugin = pChn->pHeader->nMixPlug;  		// first try intrument VST
				if ((!nPlugin) || (nPlugin > MAX_MIXPLUGINS))
					nPlugin = m_SndFile.ChnSettings[nCurrentChn+1].nMixPlugin;// Then try Channel VST
				if ((nPlugin) && (nPlugin <= MAX_MIXPLUGINS))
				{
					IMixPlugin *pPlugin =  m_SndFile.m_MixPlugins[nPlugin-1].pMixPlugin;
					if (pPlugin) pPlugin->MidiCommand(penv->nMidiChannel, penv->nMidiProgram, note+0xFF, 0, nCurrentChn);

				}
			}
		}
		//end rewbs.vstiLive

		if ((!(pChn->dwFlags & mask)) && (pChn->nLength) && ((note == pChn->nNewNote) || (!note)))
		{
			m_SndFile.KeyOff(i);
			if (!m_SndFile.m_nInstruments) pChn->dwFlags &= ~CHN_LOOP;
			if (bFade) pChn->dwFlags |= CHN_NOTEFADE;
			if (note) break;
		}
	}
	END_CRITICAL();
	return TRUE;
}


BOOL CModDoc::IsNotePlaying(UINT note, UINT nsmp, UINT nins)
//----------------------------------------------------------
{
	MODCHANNEL *pChn = &m_SndFile.Chn[m_SndFile.m_nChannels];
	for (UINT i=m_SndFile.m_nChannels; i<MAX_CHANNELS; i++, pChn++) if (!pChn->nMasterChn)
	{
		if ((pChn->nLength) && (!(pChn->dwFlags & (CHN_NOTEFADE|CHN_KEYOFF|CHN_MUTE)))
		 && ((note == pChn->nNewNote) || (!note))
		 && ((pChn->pInstrument == &m_SndFile.Ins[nsmp]) || (!nsmp))
		 && ((pChn->pHeader == m_SndFile.Headers[nins]) || (!nins))) return TRUE;
	}
	return FALSE;
}


BOOL CModDoc::MuteChannel(UINT nChn, BOOL bMute)
//----------------------------------------------
{
	DWORD d = (bMute) ? CHN_MUTE : 0;
	
	if (nChn >= m_SndFile.m_nChannels) return FALSE;
	if (d != (m_SndFile.ChnSettings[nChn].dwFlags & CHN_MUTE))
	{
		if (m_SndFile.m_nType == MOD_TYPE_IT) SetModified();
		if (d)	m_SndFile.ChnSettings[nChn].dwFlags |= CHN_MUTE;
		else	m_SndFile.ChnSettings[nChn].dwFlags &= ~CHN_MUTE;
		
	}
	if (d)	m_SndFile.Chn[nChn].dwFlags |= CHN_MUTE;
	else	m_SndFile.Chn[nChn].dwFlags &= ~CHN_MUTE;
	for (UINT i=m_SndFile.m_nChannels; i<MAX_CHANNELS; i++)
	{
		if (m_SndFile.Chn[i].nMasterChn == i+1)
		{
			if (d)	m_SndFile.Chn[i].dwFlags |= CHN_MUTE;
			else	m_SndFile.Chn[i].dwFlags &= ~CHN_MUTE;
		}
	}
	return TRUE;
}


BOOL CModDoc::MuteSample(UINT nSample, BOOL bMute)
//------------------------------------------------
{
	if ((nSample < 1) || (nSample > m_SndFile.m_nSamples)) return FALSE;
	if (bMute) m_SndFile.Ins[nSample].uFlags |= CHN_MUTE;
	else m_SndFile.Ins[nSample].uFlags &= ~CHN_MUTE;
	return TRUE;
}

BOOL CModDoc::MuteInstrument(UINT nInstr, BOOL bMute)
//---------------------------------------------------
{
	if ((nInstr < 1) || (nInstr > m_SndFile.m_nInstruments) || (!m_SndFile.Headers[nInstr])) return FALSE;
	if (bMute) m_SndFile.Headers[nInstr]->dwFlags |= ENV_MUTE;
	else m_SndFile.Headers[nInstr]->dwFlags &= ~ENV_MUTE;
	return TRUE;
}


BOOL CModDoc::SurroundChannel(UINT nChn, BOOL bSurround)
//------------------------------------------------------
{
	DWORD d = (bSurround) ? CHN_SURROUND : 0;
	
	if (nChn >= m_SndFile.m_nChannels) return FALSE;
	if (m_SndFile.m_nType != MOD_TYPE_IT) d = 0;
	if (d != (m_SndFile.ChnSettings[nChn].dwFlags & CHN_SURROUND))
	{
		if (m_SndFile.m_nType == MOD_TYPE_IT) SetModified();
		if (d)	m_SndFile.ChnSettings[nChn].dwFlags |= CHN_SURROUND;
		else	m_SndFile.ChnSettings[nChn].dwFlags &= ~CHN_SURROUND;
		
	}
	if (d)	m_SndFile.Chn[nChn].dwFlags |= CHN_SURROUND;
	else	m_SndFile.Chn[nChn].dwFlags &= ~CHN_SURROUND;
	return TRUE;
}


BOOL CModDoc::SetChannelGlobalVolume(UINT nChn, UINT nVolume)
//-----------------------------------------------------------
{
	BOOL bOk = FALSE;
	if ((nChn >= m_SndFile.m_nChannels) || (nVolume > 64)) return FALSE;
	if (m_SndFile.ChnSettings[nChn].nVolume != nVolume)
	{
		m_SndFile.ChnSettings[nChn].nVolume = nVolume;
		if (m_SndFile.m_nType & MOD_TYPE_IT) SetModified();
		bOk = TRUE;
	}
	m_SndFile.Chn[nChn].nGlobalVol = nVolume;
	return bOk;
}


BOOL CModDoc::SetChannelDefaultPan(UINT nChn, UINT nPan)
//------------------------------------------------------
{
	BOOL bOk = FALSE;
	if ((nChn >= m_SndFile.m_nChannels) || (nPan > 256)) return FALSE;
	if (m_SndFile.ChnSettings[nChn].nPan != nPan)
	{
		m_SndFile.ChnSettings[nChn].nPan = nPan;
		if (m_SndFile.m_nType & (MOD_TYPE_S3M|MOD_TYPE_IT)) SetModified();
		bOk = TRUE;
	}
	m_SndFile.Chn[nChn].nPan = nPan;
	return bOk;
}


BOOL CModDoc::IsChannelMuted(UINT nChn) const
//-------------------------------------------
{
	if (nChn >= m_SndFile.m_nChannels) return TRUE;
	return (m_SndFile.ChnSettings[nChn].dwFlags & CHN_MUTE) ? TRUE : FALSE;
}


BOOL CModDoc::IsSampleMuted(UINT nSample) const
//---------------------------------------------
{
	if ((!nSample) || (nSample > m_SndFile.m_nSamples)) return FALSE;
	return (m_SndFile.Ins[nSample].uFlags & CHN_MUTE) ? TRUE : FALSE;
}


BOOL CModDoc::IsInstrumentMuted(UINT nInstr) const
//------------------------------------------------
{
	if ((!nInstr) || (nInstr > m_SndFile.m_nInstruments) || (!m_SndFile.Headers[nInstr])) return FALSE;
	return (m_SndFile.Headers[nInstr]->dwFlags & ENV_MUTE) ? TRUE : FALSE;
}


UINT CModDoc::GetPatternSize(UINT nPat) const
//-------------------------------------------
{
	if ((nPat < MAX_PATTERNS) && (m_SndFile.Patterns[nPat])) return m_SndFile.PatternSize[nPat];
	return 0;
}


void CModDoc::SetFollowWnd(HWND hwnd, DWORD dwType)
//-------------------------------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	m_hWndFollow = hwnd;
	m_dwNotifyType = dwType;
	if (pMainFrm) pMainFrm->SetFollowSong(this, m_hWndFollow, TRUE, m_dwNotifyType);
}


BOOL CModDoc::IsChildSample(UINT nIns, UINT nSmp) const
//-----------------------------------------------------
{
	INSTRUMENTHEADER *penv;
	if ((nIns < 1) || (nIns > m_SndFile.m_nInstruments)) return FALSE;
	penv = m_SndFile.Headers[nIns];
	if (penv)
	{
		for (UINT i=0; i<120; i++)
		{
			if (penv->Keyboard[i] == nSmp) return TRUE;
		}
	}
	return FALSE;
}


UINT CModDoc::FindSampleParent(UINT nSmp) const
//---------------------------------------------
{
	if ((!m_SndFile.m_nInstruments) || (!nSmp)) return 0;
	for (UINT i=1; i<=m_SndFile.m_nInstruments; i++)
	{
		INSTRUMENTHEADER *penv = m_SndFile.Headers[i];
		if (penv)
		{
			for (UINT j=0; j<120; j++)
			{
				if (penv->Keyboard[j] == nSmp) return i;
			}
		}
	}
	return 0;
}


UINT CModDoc::FindInstrumentChild(UINT nIns) const
//------------------------------------------------
{
	INSTRUMENTHEADER *penv;
	if ((!nIns) || (nIns > m_SndFile.m_nInstruments)) return 0;
	penv = m_SndFile.Headers[nIns];
	if (penv)
	{
		for (UINT i=0; i<120; i++)
		{
			UINT n = penv->Keyboard[i];
			if ((n) && (n <= m_SndFile.m_nSamples)) return n;
		}
	}
	return 0;
}


LRESULT CModDoc::ActivateView(UINT nIdView, DWORD dwParam)
//--------------------------------------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (!pMainFrm) return 0;
	CMDIChildWnd *pMDIActive = pMainFrm->MDIGetActive();
	if (pMDIActive)
	{
		CView *pView = pMDIActive->GetActiveView();
		if ((pView) && (pView->GetDocument() == this))
		{
			return ((CChildFrame *)pMDIActive)->ActivateView(nIdView, dwParam);
		}
	}
	POSITION pos = GetFirstViewPosition();
	while (pos != NULL)
	{
		CView *pView = GetNextView(pos);
		if ((pView) && (pView->GetDocument() == this))
		{
			CChildFrame *pChildFrm = (CChildFrame *)pView->GetParentFrame();
			pChildFrm->MDIActivate();
			return pChildFrm->ActivateView(nIdView, dwParam);
		}
	}
	return 0;
}


void CModDoc::UpdateAllViews(CView *pSender, LPARAM lHint, CObject *pHint)
//------------------------------------------------------------------------
{
	CDocument::UpdateAllViews(pSender, lHint, pHint);
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm) pMainFrm->UpdateTree(this, lHint, pHint);
}


/////////////////////////////////////////////////////////////////////////////
// CModDoc commands

void CModDoc::OnFileWaveConvert()
//-------------------------------
{
	CHAR path[_MAX_PATH]="", drive[_MAX_DRIVE]="";
	CHAR s[_MAX_PATH], fname[_MAX_FNAME]="";
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();

	if ((!pMainFrm) || (!m_SndFile.GetType())) return;
	_splitpath(GetPathName(), drive, path, fname, NULL);
	strcpy(s, fname);
	strcat(s, ".wav");
	CFileDialog dlg(FALSE, "wav", s,
					OFN_HIDEREADONLY | OFN_ENABLESIZING | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN,
					"Wave Files (*.wav)|*.wav||", pMainFrm);
	dlg.m_ofn.lpstrInitialDir = pMainFrm->GetExportDir();
	if (dlg.DoModal() == IDOK)
	{
		s[0] = 0;
		_splitpath(dlg.GetPathName(), s, path, NULL, NULL);
		strcat(s, path);
		pMainFrm->SetExportDir(s);
		strcpy(s, dlg.GetPathName());
		CWaveConvert wsdlg(pMainFrm);
		if (wsdlg.DoModal() != IDOK) return;
		// Saving as wave file
		BOOL bplaying = FALSE;
		UINT pos = m_SndFile.GetCurrentPos();
		bplaying = TRUE;
		pMainFrm->PauseMod();
		m_SndFile.SetCurrentPos(0);
		if (wsdlg.m_bSelectPlay)
		{
			m_SndFile.SetCurrentOrder(wsdlg.m_nMinOrder);
			m_SndFile.m_nCurrentPattern = wsdlg.m_nMinOrder;
			m_SndFile.GetLength(TRUE, FALSE);
			m_SndFile.m_nMaxOrderPosition = wsdlg.m_nMaxOrder + 1;
		}
		// Saving file
		CDoWaveConvert dwcdlg(&m_SndFile, s, &wsdlg.WaveFormat.Format, wsdlg.m_bNormalize, pMainFrm);
		dwcdlg.m_dwFileLimit = wsdlg.m_dwFileLimit;
		dwcdlg.m_dwSongLimit = wsdlg.m_dwSongLimit;
		dwcdlg.m_nMaxPatterns = (wsdlg.m_bSelectPlay) ? wsdlg.m_nMaxOrder - wsdlg.m_nMinOrder + 1 : 0;
		if (wsdlg.m_bHighQuality)
		{
			CSoundFile::SetResamplingMode(SRCMODE_POLYPHASE);
		}
		dwcdlg.DoModal();
		m_SndFile.SetCurrentPos(pos);
		m_SndFile.GetLength(TRUE);
		CMainFrame::UpdateAudioParameters(TRUE);
	}
}


void CModDoc::OnFileMP3Convert()
//------------------------------
{
	CHAR path[_MAX_PATH]="", drive[_MAX_DRIVE]="";
	CHAR s[_MAX_PATH], fname[_MAX_FNAME]="", fext[_MAX_EXT] = "";
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();

	if ((!pMainFrm) || (!m_SndFile.GetType())) return;
	_splitpath(GetPathName(), drive, path, fname, NULL);
	strcpy(s, drive);
	strcat(s, path);
	strcat(s, fname);
	CFileDialog dlg(FALSE, "mp3", s,
					OFN_HIDEREADONLY | OFN_ENABLESIZING | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN,
					"MPEG Layer III Files (*.mp3)|*.mp3|Layer3 Wave Files (*.wav)|*.wav||", pMainFrm);
	dlg.m_ofn.nFilterIndex = 0;
	dlg.m_ofn.lpstrInitialDir = pMainFrm->GetExportDir();
	if (dlg.DoModal() == IDOK)
	{
		MPEGLAYER3WAVEFORMAT wfx;
		HACMDRIVERID hadid;

		s[0] = 0;
		_splitpath(dlg.GetPathName(), s, path, NULL, NULL);
		strcat(s, path);
		pMainFrm->SetExportDir(s);
		strcpy(s, dlg.GetPathName());
		_splitpath(s, NULL, NULL, NULL, fext);
		if (strlen(fext) <= 1)
		{
			int l = strlen(s) - 1;
			if ((l >= 0) && (s[l] == '.')) s[l] = 0;
			strcpy(fext, (dlg.m_ofn.nFilterIndex == 2) ? ".wav" : ".mp3");
			strcat(s, fext);
		}
		CLayer3Convert wsdlg(&m_SndFile, pMainFrm);
		if (m_SndFile.m_szNames[0][0]) wsdlg.m_bSaveInfoField = TRUE;
		if (wsdlg.DoModal() != IDOK) return;
		wsdlg.GetFormat(&wfx, &hadid);
		// Saving as mpeg file
		BOOL bplaying = FALSE;
		UINT pos = m_SndFile.GetCurrentPos();
		bplaying = TRUE;
		pMainFrm->PauseMod();
		m_SndFile.SetCurrentPos(0);
		// Saving file
		PTAGID3INFO pTag = (wsdlg.m_bSaveInfoField) ? &wsdlg.m_id3tag : NULL;
		CDoAcmConvert dwcdlg(&m_SndFile, s, &wfx.wfx, hadid, pTag, pMainFrm);
		dwcdlg.m_dwFileLimit = wsdlg.m_dwFileLimit;
		dwcdlg.m_dwSongLimit = wsdlg.m_dwSongLimit;
		dwcdlg.DoModal();
		m_SndFile.SetCurrentPos(pos);
		m_SndFile.GetLength(TRUE);
		CMainFrame::UpdateAudioParameters(TRUE);
	}
}

 
void CModDoc::OnFileMidiConvert()
//-------------------------------
{
	CHAR path[_MAX_PATH]="", drive[_MAX_DRIVE]="";
	CHAR s[_MAX_PATH], fname[_MAX_FNAME]="";
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();

	if ((!pMainFrm) || (!m_SndFile.GetType())) return;
	_splitpath(GetPathName(), drive, path, fname, NULL);
	strcpy(s, drive);
	strcat(s, path);
	strcat(s, fname);
	strcat(s, ".mid");
	CFileDialog dlg(FALSE, "mid", s,
					OFN_HIDEREADONLY | OFN_ENABLESIZING | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN,
					"Midi Files (*.mid,*.rmi)|*.mid;*.rmi||", pMainFrm);
	dlg.m_ofn.nFilterIndex = 0;
	if (dlg.DoModal() == IDOK)
	{
		CModToMidi mididlg(dlg.GetPathName(), &m_SndFile, pMainFrm);
		if (mididlg.DoModal() == IDOK)
		{
			BeginWaitCursor();
			mididlg.DoConvert();
			EndWaitCursor();
		}
	}
}


void CModDoc::OnPlayerPlay()
//--------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm)
	{
		BOOL bPlaying = (pMainFrm->GetModPlaying() == this) ? TRUE : FALSE;
		if ((bPlaying) && (!(m_SndFile.m_dwSongFlags & (SONG_PAUSED|SONG_STEP/*|SONG_PATTERNLOOP*/))))
		{
			OnPlayerPause();
			return;
		}
		BEGIN_CRITICAL();
		for (UINT i=m_SndFile.m_nChannels; i<MAX_CHANNELS; i++) if (!m_SndFile.Chn[i].nMasterChn)
		{
			m_SndFile.Chn[i].dwFlags |= (CHN_NOTEFADE|CHN_KEYOFF);
			if (!bPlaying) m_SndFile.Chn[i].nLength = 0;
		}
		END_CRITICAL();
		m_SndFile.m_dwSongFlags &= ~(SONG_STEP|SONG_PAUSED);
		pMainFrm->PlayMod(this, m_hWndFollow, m_dwNotifyType);
	}
}


void CModDoc::OnPlayerPause()
//---------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm)
	{
		if (pMainFrm->GetModPlaying() == this)
		{
			BOOL bLoop = (m_SndFile.m_dwSongFlags & SONG_PATTERNLOOP) ? TRUE : FALSE;
			UINT nPat = m_SndFile.m_nPattern;
			UINT nRow = m_SndFile.m_nRow;
			UINT nNextRow = m_SndFile.m_nNextRow;
			pMainFrm->PauseMod();
			BEGIN_CRITICAL();
			if ((bLoop) && (nPat < MAX_PATTERNS))
			{
				if ((m_SndFile.m_nCurrentPattern < MAX_ORDERS) && (m_SndFile.Order[m_SndFile.m_nCurrentPattern] == nPat))
				{
					m_SndFile.m_nNextPattern = m_SndFile.m_nCurrentPattern;
					m_SndFile.m_nNextRow = nNextRow;
					m_SndFile.m_nRow = nRow;
				} else
				{
					for (UINT i=0; i<MAX_ORDERS; i++)
					{
						if (m_SndFile.Order[i] == 0xFF) break;
						if (m_SndFile.Order[i] == nPat)
						{
							m_SndFile.m_nCurrentPattern = i;
							m_SndFile.m_nNextPattern = i;
							m_SndFile.m_nNextRow = nNextRow;
							m_SndFile.m_nRow = nRow;
							break;
						}
					}
				}
			}
			END_CRITICAL();
		} else
		{
			pMainFrm->PauseMod();
		}
	}
}


void CModDoc::OnPlayerStop()
//--------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm) pMainFrm->StopMod();
}


void CModDoc::OnPlayerPlayFromStart()
//-----------------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm)
	{
		pMainFrm->PauseMod();
		m_SndFile.m_dwSongFlags &= ~SONG_STEP;
		m_SndFile.SetCurrentPos(0);
		pMainFrm->ResetElapsedTime();
		pMainFrm->PlayMod(this, m_hWndFollow, m_dwNotifyType);
		CMainFrame::EnableLowLatencyMode(FALSE);
	}
}
/*
void CModDoc::OnPlayerPlayFromPos(UINT pos)
//-----------------------------------
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (pMainFrm)
	{
		pMainFrm->PauseMod();
		m_SndFile.m_dwSongFlags &= ~SONG_STEP;
		m_SndFile.SetCurrentPos(pos);
		pMainFrm->ResetElapsedTime();
		pMainFrm->PlayMod(this, m_hWndFollow, m_dwNotifyType);
		CMainFrame::EnableLowLatencyMode(FALSE);
	}
}
*/

void CModDoc::OnEditGlobals()
//---------------------------
{
	SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW,	IDD_CONTROL_GLOBALS);
}


void CModDoc::OnEditPatterns()
//----------------------------
{
	SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW, IDD_CONTROL_PATTERNS, -1);
}


void CModDoc::OnEditSamples()
//---------------------------
{
	SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW, IDD_CONTROL_SAMPLES);
}


void CModDoc::OnEditInstruments()
//-------------------------------
{
	if (m_SndFile.m_nInstruments) SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW, IDD_CONTROL_INSTRUMENTS);
}


void CModDoc::OnEditComments()
//----------------------------
{
	if (m_SndFile.m_nType & (MOD_TYPE_XM|MOD_TYPE_IT)) SendMessageToActiveViews(WM_MOD_ACTIVATEVIEW, IDD_CONTROL_COMMENTS);
}


void CModDoc::OnCleanupSamples()
//------------------------------
{
	ClearLog();
	RemoveUnusedSamples();
	UpdateAllViews(NULL, HINT_MODTYPE);
	ShowLog("Sample Cleanup", CMainFrame::GetMainFrame());
}


void CModDoc::OnCleanupInstruments()
//----------------------------------
{
	ClearLog();
	RemoveUnusedInstruments();
	UpdateAllViews(NULL, HINT_MODTYPE);
	ShowLog("Instrument Cleanup", CMainFrame::GetMainFrame());
}


void CModDoc::OnCleanupPatterns()
//-------------------------------
{
	ClearLog();
	RemoveUnusedPatterns();
	UpdateAllViews(NULL, HINT_MODTYPE|HINT_MODSEQUENCE);
	ShowLog("Pattern Cleanup", CMainFrame::GetMainFrame());
}


void CModDoc::OnCleanupSong()
//---------------------------
{
	ClearLog();
	RemoveUnusedPatterns();
	RemoveUnusedInstruments();
	RemoveUnusedSamples();
	UpdateAllViews(NULL, HINT_MODTYPE|HINT_MODSEQUENCE);
	ShowLog("Song Cleanup", CMainFrame::GetMainFrame());
}


void CModDoc::OnRearrangePatterns()
//---------------------------------
{
	ClearLog();
	RemoveUnusedPatterns(FALSE);
	UpdateAllViews(NULL, HINT_MODTYPE|HINT_MODSEQUENCE);
	ShowLog("Pattern Rearrange", CMainFrame::GetMainFrame());
}


void CModDoc::OnUpdateInstrumentOnly(CCmdUI *p)
//---------------------------------------------
{
	if (p) p->Enable((m_SndFile.m_nInstruments) ? TRUE : FALSE);
}


void CModDoc::OnUpdateXMITOnly(CCmdUI *p)
//---------------------------------------
{
	if (p) p->Enable(((m_SndFile.m_nType == MOD_TYPE_XM) 
		|| (m_SndFile.m_nType == MOD_TYPE_IT)) ? TRUE : FALSE);
}


void CModDoc::OnUpdateMP3Encode(CCmdUI *p)
//----------------------------------------
{
	if (p) p->Enable(theApp.CanEncodeLayer3());
}


void CModDoc::OnInsertPattern()
//-----------------------------
{
	LONG pat = InsertPattern();
	if (pat >= 0)
	{
		UINT ord = 0;
		for (UINT i=0; i<MAX_ORDERS; i++)
		{
			if (m_SndFile.Order[i] == pat) ord = i;
			if (m_SndFile.Order[i] == 0xFF) break;
		}
		ViewPattern(pat, ord);
	}
}


void CModDoc::OnInsertSample()
//----------------------------
{
	LONG smp = InsertSample();
	if (smp >= 0) ViewSample(smp);
}


void CModDoc::OnInsertInstrument()
//--------------------------------
{
	LONG ins = InsertInstrument();
	if (ins >= 0) ViewInstrument(ins);
}


void CModDoc::OnRemoveAllInstruments()
//------------------------------------
{
	if (!m_SndFile.m_nInstruments) return;
	if (CMainFrame::GetMainFrame()->MessageBox("This will remove all the instruments in the song,\n"
		"Do you want to continue?", "Warning", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
	if (CMainFrame::GetMainFrame()->MessageBox("Do you want to convert all instruments to samples ?\n",
		NULL, MB_YESNO | MB_ICONQUESTION) == IDYES)
	{
		ConvertInstrumentsToSamples();
	}
	for (UINT i=1; i<=m_SndFile.m_nInstruments; i++)
	{
		m_SndFile.DestroyInstrument(i);
	}
	m_SndFile.m_nInstruments = 0;
	SetModified();
	UpdateAllViews(NULL, HINT_MODTYPE);
}


void CModDoc::OnEstimateSongLength()
//----------------------------------
{
	CHAR s[256];
	DWORD dwSongLength = m_SndFile.GetLength(FALSE, TRUE);
	wsprintf(s, "Approximate song length: %dmn%02ds", dwSongLength/60, dwSongLength%60);
	CMainFrame::GetMainFrame()->MessageBox(s, NULL, MB_OK|MB_ICONINFORMATION);
}


///////////////////////////////////////////////////////////////////////////
// Effects description

typedef struct MPTEFFECTINFO
{
	DWORD dwEffect;		// CMD_XXXX
	DWORD dwParamMask;	// 0 = default
	DWORD dwParamValue;	// 0 = default
	DWORD dwFlags;		// FXINFO_XXXX
	DWORD dwFormats;	// MOD_TYPE_XXX combo
	LPCSTR pszName;		// ie "Tone Portamento"
} MPTEFFECTINFO;

#define MOD_TYPE_MODXM	(MOD_TYPE_MOD|MOD_TYPE_XM)
#define MOD_TYPE_S3MIT	(MOD_TYPE_S3M|MOD_TYPE_IT)
#define MOD_TYPE_NOMOD	(MOD_TYPE_S3M|MOD_TYPE_XM|MOD_TYPE_IT)
#define MOD_TYPE_XMIT	(MOD_TYPE_XM|MOD_TYPE_IT)
#define MAX_FXINFO		65					//rewbs.smoothVST, increased from 64... I wonder what this will break?

const MPTEFFECTINFO gFXInfo[MAX_FXINFO] =
{
	{CMD_ARPEGGIO,		0,0,		0,	0xFFFFFFFF,		"Arpeggio"},
	{CMD_PORTAMENTOUP,	0,0,		0,	0xFFFFFFFF,		"Portamento Up"},
	{CMD_PORTAMENTODOWN,0,0,		0,	0xFFFFFFFF,		"Portamento Down"},
	{CMD_TONEPORTAMENTO,0,0,		0,	0xFFFFFFFF,		"Tone portamento"},
	{CMD_VIBRATO,		0,0,		0,	0xFFFFFFFF,		"Vibrato"},
	{CMD_TONEPORTAVOL,	0,0,		0,	0xFFFFFFFF,		"Volslide+Toneporta"},
	{CMD_VIBRATOVOL,	0,0,		0,	0xFFFFFFFF,		"VolSlide+Vibrato"},
	{CMD_TREMOLO,		0,0,		0,	0xFFFFFFFF,		"Tremolo"},
	{CMD_PANNING8,		0,0,		0,	0xFFFFFFFF,		"Set Panning"},
	{CMD_OFFSET,		0,0,		0,	0xFFFFFFFF,		"Set Offset"},
	{CMD_VOLUMESLIDE,	0,0,		0,	0xFFFFFFFF,		"Volume Slide"},
	{CMD_POSITIONJUMP,	0,0,		0,	0xFFFFFFFF,		"Position Jump"},
	{CMD_VOLUME,		0,0,		0,	MOD_TYPE_MODXM,	"Set Volume"},
	{CMD_PATTERNBREAK,	0,0,		0,	0xFFFFFFFF,		"Pattern Break"},
	{CMD_RETRIG,		0,0,		0,	MOD_TYPE_NOMOD,	"Retrigger Note"},
	{CMD_SPEED,			0,0,		0,	0xFFFFFFFF,		"Set Speed"},
	{CMD_TEMPO,			0,0,		0,	0xFFFFFFFF,		"Set Tempo"},
	{CMD_TREMOR,		0,0,		0,	MOD_TYPE_NOMOD,	"Tremor"},
	{CMD_CHANNELVOLUME,	0,0,		0,	MOD_TYPE_S3MIT,	"Set channel volume"},
	{CMD_CHANNELVOLSLIDE,0,0,		0,	MOD_TYPE_S3MIT,	"Channel volslide"},
	{CMD_GLOBALVOLUME,	0,0,		0,	MOD_TYPE_NOMOD,	"Set global volume"},
	{CMD_GLOBALVOLSLIDE,0,0,		0,	MOD_TYPE_NOMOD,	"Global volume slide"},
	{CMD_KEYOFF,		0,0,		0,	MOD_TYPE_XM,	"Key off"},
	{CMD_FINEVIBRATO,	0,0,		0,	MOD_TYPE_S3MIT,	"Fine vibrato"},
	{CMD_PANBRELLO,		0,0,		0,	MOD_TYPE_NOMOD,	"Panbrello"},
	{CMD_PANNINGSLIDE,	0,0,		0,	MOD_TYPE_NOMOD,	"Panning slide"},
	{CMD_SETENVPOSITION,0,0,		0,	MOD_TYPE_XM,	"Envelope position"},
	{CMD_MIDI,			0,0,		0,	MOD_TYPE_NOMOD,	"Midi macro"},
	{CMD_SMOOTHMIDI,	0,0,		0,	MOD_TYPE_NOMOD,	"Smooth Midi macro"},	//rewbs.smoothVST
	// Extended MOD/XM effects
	{CMD_MODCMDEX,		0xF0,0x10,	0,	MOD_TYPE_MODXM,	"Fine porta up"},
	{CMD_MODCMDEX,		0xF0,0x20,	0,	MOD_TYPE_MODXM,	"Fine porta down"},
	{CMD_MODCMDEX,		0xF0,0x30,	0,	MOD_TYPE_MODXM,	"Glissando Control"},
	{CMD_MODCMDEX,		0xF0,0x40,	0,	MOD_TYPE_MODXM,	"Vibrato waveform"},
	{CMD_MODCMDEX,		0xF0,0x50,	0,	MOD_TYPE_MOD,	"Set finetune"},
	{CMD_MODCMDEX,		0xF0,0x60,	0,	MOD_TYPE_MODXM,	"Pattern loop"},
	{CMD_MODCMDEX,		0xF0,0x70,	0,	MOD_TYPE_MODXM,	"Tremolo waveform"},
	{CMD_MODCMDEX,		0xF0,0x80,	0,	MOD_TYPE_MODXM,	"Set panning"},
	{CMD_MODCMDEX,		0xF0,0x90,	0,	MOD_TYPE_MODXM,	"Retrigger note"},
	{CMD_MODCMDEX,		0xF0,0xA0,	0,	MOD_TYPE_MODXM,	"Fine volslide up"},
	{CMD_MODCMDEX,		0xF0,0xB0,	0,	MOD_TYPE_MODXM,	"Fine volslide down"},
	{CMD_MODCMDEX,		0xF0,0xC0,	0,	MOD_TYPE_MODXM,	"Note cut"},
	{CMD_MODCMDEX,		0xF0,0xD0,	0,	MOD_TYPE_MODXM,	"Note delay"},
	{CMD_MODCMDEX,		0xF0,0xE0,	0,	MOD_TYPE_MODXM,	"Pattern delay"},
	{CMD_MODCMDEX,		0xF0,0xF0,	0,	MOD_TYPE_XM,	"Set active macro"},
	// Extended S3M/IT effects
	{CMD_S3MCMDEX,		0xF0,0x10,	0,	MOD_TYPE_S3MIT,	"Glissando control"},
	{CMD_S3MCMDEX,		0xF0,0x20,	0,	MOD_TYPE_S3M,	"Set finetune"},
	{CMD_S3MCMDEX,		0xF0,0x30,	0,	MOD_TYPE_S3MIT,	"Vibrato waveform"},
	{CMD_S3MCMDEX,		0xF0,0x40,	0,	MOD_TYPE_S3MIT,	"Tremolo waveform"},
	{CMD_S3MCMDEX,		0xF0,0x50,	0,	MOD_TYPE_S3MIT,	"Panbrello waveform"},
	{CMD_S3MCMDEX,		0xF0,0x60,	0,	MOD_TYPE_S3MIT,	"Fine pattern delay"},
	{CMD_S3MCMDEX,		0xF0,0x80,	0,	MOD_TYPE_S3MIT,	"Set panning"},
	{CMD_S3MCMDEX,		0xF0,0xA0,	0,	MOD_TYPE_S3MIT,	"Set high offset"},
	{CMD_S3MCMDEX,		0xF0,0xB0,	0,	MOD_TYPE_S3MIT,	"Pattern loop"},
	{CMD_S3MCMDEX,		0xF0,0xC0,	0,	MOD_TYPE_S3MIT,	"Note cut"},
	{CMD_S3MCMDEX,		0xF0,0xD0,	0,	MOD_TYPE_S3MIT,	"Note delay"},
	{CMD_S3MCMDEX,		0xF0,0xE0,	0,	MOD_TYPE_S3MIT,	"Pattern delay"},
	{CMD_S3MCMDEX,		0xF0,0xF0,	0,	MOD_TYPE_IT,	"Set active macro"},
	// MPT XM extensions and special effects
	{CMD_XFINEPORTAUPDOWN,0xF0,0x10,0,	MOD_TYPE_XM,	"Extra fine porta up"},
	{CMD_XFINEPORTAUPDOWN,0xF0,0x20,0,	MOD_TYPE_XM,	"Extra fine porta down"},
	{CMD_XFINEPORTAUPDOWN,0xF0,0x50,0,	MOD_TYPE_XM,	"Panbrello waveform"},
	{CMD_XFINEPORTAUPDOWN,0xF0,0x60,0,	MOD_TYPE_XM,	"Fine pattern delay"},
	{CMD_XFINEPORTAUPDOWN,0xF0,0x90,0,	MOD_TYPE_XM,	"Sound control"},
	{CMD_XFINEPORTAUPDOWN,0xF0,0xA0,0,	MOD_TYPE_XM,	"Set high offset"},
	// MPT IT extensions and special effects
	{CMD_S3MCMDEX,		0xF0,0x90,	0,	MOD_TYPE_S3MIT,	"Sound control"},
	{CMD_S3MCMDEX,		0xF0,0x70,	0,	MOD_TYPE_IT,	"Instr. control"},
};


UINT CModDoc::GetNumEffects() const
//---------------------------------
{
	return MAX_FXINFO;
}


BOOL CModDoc::IsExtendedEffect(UINT ndx) const
//--------------------------------------------
{
	return ((ndx < MAX_FXINFO) && (gFXInfo[ndx].dwParamMask));
}


BOOL CModDoc::GetEffectName(LPSTR pszDescription, UINT command, UINT param, BOOL bXX)
//-----------------------------------------------------------------------------------
{
	BOOL bSupported;
	int fxndx = -1;
	pszDescription[0] = 0;
	for (UINT i=0; i<MAX_FXINFO; i++)
	{
		if ((command == gFXInfo[i].dwEffect) // Effect
		 && ((param & gFXInfo[i].dwParamMask) == gFXInfo[i].dwParamValue)) // Value
		{
			fxndx = i;
			break;
		}
	}
	if (fxndx < 0) return FALSE;
	bSupported = ((m_SndFile.m_nType & gFXInfo[fxndx].dwFormats) != 0);
	if (gFXInfo[fxndx].pszName)
	{
		if ((bXX) && (bSupported))
		{
			strcpy(pszDescription, " xx: ");
			LPCSTR pszCmd = (m_SndFile.m_nType & (MOD_TYPE_MOD|MOD_TYPE_XM)) ? gszModCommands : gszS3mCommands;
			pszDescription[0] = pszCmd[command];
			if ((gFXInfo[fxndx].dwParamMask & 0xF0) == 0xF0) pszDescription[1] = szHexChar[gFXInfo[i].dwParamValue >> 4];
			if ((gFXInfo[fxndx].dwParamMask & 0x0F) == 0x0F) pszDescription[2] = szHexChar[gFXInfo[i].dwParamValue & 0x0F];
		}
		strcat(pszDescription, gFXInfo[fxndx].pszName);
	}
	return bSupported;
}


LONG CModDoc::GetIndexFromEffect(UINT command, UINT param)
//--------------------------------------------------------
{
	for (UINT i=0; i<MAX_FXINFO; i++)
	{
		if ((command == gFXInfo[i].dwEffect) // Effect
		 && ((param & gFXInfo[i].dwParamMask) == gFXInfo[i].dwParamValue)) // Value
		{
			return i;
		}
	}
	return -1;
}


UINT CModDoc::GetEffectFromIndex(UINT ndx, int *pParam)
//-----------------------------------------------------
{
	//if (pParam) *pParam = -1;
	if (ndx >= MAX_FXINFO)
	{
		if (pParam) *pParam = 0;
		return 0;
	}
	if ((pParam) && (gFXInfo[ndx].dwParamMask))
	{
//		if ((*pParam < gFXInfo[ndx].dwParamValue) || (*pParam > gFXInfo[ndx].dwParamValue+15))
//			*pParam = gFXInfo[ndx].dwParamValue + ((*pParam)/16);
		if (*pParam < gFXInfo[ndx].dwParamValue)
			*pParam = gFXInfo[ndx].dwParamValue;
		else if (*pParam > gFXInfo[ndx].dwParamValue+15)
			*pParam = gFXInfo[ndx].dwParamValue+15;
	}


	return gFXInfo[ndx].dwEffect;
}


BOOL CModDoc::GetEffectInfo(UINT ndx, LPSTR s, BOOL bXX, DWORD *prangeMin, DWORD *prangeMax)
//------------------------------------------------------------------------------------------
{
	if (s) s[0] = 0;
	if (prangeMin) *prangeMin = 0;
	if (prangeMax) *prangeMax = 0;
	if ((ndx >= MAX_FXINFO) || (!(m_SndFile.m_nType & gFXInfo[ndx].dwFormats))) return FALSE;
	if (s) GetEffectName(s, gFXInfo[ndx].dwEffect, gFXInfo[ndx].dwParamValue, bXX);
	if ((prangeMin) && (prangeMax))
	{
		UINT nmin = 0, nmax = 0xFF, nType = m_SndFile.m_nType;
		if (gFXInfo[ndx].dwParamMask == 0xF0)
		{
			nmin = gFXInfo[ndx].dwParamValue;
			nmax = nmin | 0x0F;
		}
		switch(gFXInfo[ndx].dwEffect)
		{
		case CMD_ARPEGGIO:
			if (nType & MOD_TYPE_MOD) nmin = 1;
			break;
		case CMD_VOLUME:
			nmax = 0x40;
			break;
		case CMD_SPEED:
			nmin = 1;
			nmax = 0x7F;
			if (nType & MOD_TYPE_MOD) nmax = 0x20; else
			if (nType & MOD_TYPE_XM) nmax = 0x1F;
			break;
		case CMD_TEMPO:
			nmin = 0x20;
			if (nType & MOD_TYPE_MOD) nmin = 0x21; else
			if (nType & MOD_TYPE_S3MIT) nmin = 1;
			break;
		case CMD_VOLUMESLIDE:
		case CMD_TONEPORTAVOL:
		case CMD_VIBRATOVOL:
		case CMD_GLOBALVOLSLIDE:
		case CMD_CHANNELVOLSLIDE:
			nmax = (nType & MOD_TYPE_S3MIT) ? 58 : 30;
			break;
		case CMD_PANNING8:
			if (nType & (MOD_TYPE_MOD|MOD_TYPE_S3M)) nmax = 0x80;
			break;
		case CMD_GLOBALVOLUME:
			nmax = (nType & MOD_TYPE_IT) ? 128 : 64;
			break;
		}
		*prangeMin = nmin;
		*prangeMax = nmax;
	}
	return TRUE;
}


UINT CModDoc::MapValueToPos(UINT ndx, UINT param)
//-----------------------------------------------
{
	UINT pos;
	
	if (ndx >= MAX_FXINFO) return 0;
	pos = param;
	if (gFXInfo[ndx].dwParamMask == 0xF0)
	{
		pos &= 0x0F;
		pos |= gFXInfo[ndx].dwParamValue;
	}
	switch(gFXInfo[ndx].dwEffect)
	{
	case CMD_VOLUMESLIDE:
	case CMD_TONEPORTAVOL:
	case CMD_VIBRATOVOL:
	case CMD_GLOBALVOLSLIDE:
	case CMD_CHANNELVOLSLIDE:
		if (m_SndFile.m_nType & MOD_TYPE_S3MIT)
		{
			if (!param) pos = 29; else
			if (((param & 0x0F) == 0x0F) && (param & 0xF0))
				pos = 29 + (param >> 4); else
			if (((param & 0xF0) == 0xF0) && (param & 0x0F))
				pos = 29 - (param & 0x0F); else
			if (param & 0x0F)
				pos = 15 - (param & 0x0F);
			else
				pos = (param >> 4) + 44;
		} else
		{
			if (param & 0x0F) pos = 15 - (param & 0x0F);
			else pos = (param >> 4) + 15;
		}
		break;
	}
	return pos;
}


UINT CModDoc::MapPosToValue(UINT ndx, UINT pos)
//---------------------------------------------
{
	UINT param;
	
	if (ndx >= MAX_FXINFO) return 0;
	param = pos;
	if (gFXInfo[ndx].dwParamMask == 0xF0) param |= gFXInfo[ndx].dwParamValue;
	switch(gFXInfo[ndx].dwEffect)
	{
	case CMD_VOLUMESLIDE:
	case CMD_TONEPORTAVOL:
	case CMD_VIBRATOVOL:
	case CMD_GLOBALVOLSLIDE:
	case CMD_CHANNELVOLSLIDE:
		if (m_SndFile.m_nType & MOD_TYPE_S3MIT)
		{
			if (pos < 15) param = 15-pos; else
			if (pos < 29) param = (29-pos) | 0xF0; else
			if (pos == 29) param = 0; else
			if (pos < 44) param = ((pos - 29) << 4) | 0x0F; else
			if (pos < 59) param = (pos-43) << 4;
		} else
		{
			if (pos < 15) param = 15 - pos; else
			param = (pos - 15) << 4;
		}
		break;
	}
	return param;
}


BOOL CModDoc::GetEffectNameEx(LPSTR pszName, UINT ndx, UINT param)
//----------------------------------------------------------------
{
	CHAR s[64];
	if (pszName) pszName[0] = 0;
	if ((!pszName) || (ndx >= MAX_FXINFO) || (!gFXInfo[ndx].pszName)) return FALSE;
	wsprintf(pszName, "%s: ", gFXInfo[ndx].pszName);
	s[0] = 0;
	switch(gFXInfo[ndx].dwEffect)
	{
	case CMD_ARPEGGIO:
		if (param)
			wsprintf(s, "note+%d note+%d", param >> 4, param & 0x0F);
		else
			wsprintf(s, "continue");
		break;

	case CMD_PORTAMENTOUP:
		if (param)
			wsprintf(s, "+%d", param);
		else
			wsprintf(s, "continue");
		break;

	case CMD_PORTAMENTODOWN:
		if (param)
			wsprintf(s, "-%d", param);
		else
			wsprintf(s, "continue");
		break;

	case CMD_TONEPORTAMENTO:
		if (param)
			wsprintf(s, "speed %d", param);
		else
			wsprintf(s, "continue");
		break;

	case CMD_VIBRATO:
	case CMD_TREMOLO:
	case CMD_PANBRELLO:
	case CMD_FINEVIBRATO:
		if (param)
			wsprintf(s, "speed=%d depth=%d", param >> 4, param & 0x0F);
		else
			strcpy(s, "continue");
		break;

	case CMD_SPEED:
		wsprintf(s, "%d frames/row", param);
		break;

	case CMD_TEMPO:
		if (param < 0x10)
			wsprintf(s, "-%dbpm (slower)", param & 0x0F);
		else if (param < 0x20)
			wsprintf(s, "+%dbpm (faster)", param & 0x0F);
		else
			wsprintf(s, "%dbpm", param);
		break;

	case CMD_VOLUMESLIDE:
	case CMD_TONEPORTAVOL:
	case CMD_VIBRATOVOL:
	case CMD_GLOBALVOLSLIDE:
	case CMD_CHANNELVOLSLIDE:
		if (!param)
		{
			wsprintf(s, "continue");
		} else
		if ((m_SndFile.m_nType & MOD_TYPE_S3MIT) && ((param & 0x0F) == 0x0F) && (param & 0xF0))
		{
			wsprintf(s, "fine +%d", param >> 4);
		} else
		if ((m_SndFile.m_nType & MOD_TYPE_S3MIT) && ((param & 0xF0) == 0xF0) && (param & 0x0F))
		{
			wsprintf(s, "fine -%d", param & 0x0F);
		} else
		if (param & 0x0F)
		{
			wsprintf(s, "-%d", param & 0x0F);
		} else
		{
			wsprintf(s, "+%d", param >> 4);
		}
		break;

	case CMD_PATTERNBREAK:
		wsprintf(pszName, "Break to row %d", param);
		break;

	case CMD_POSITIONJUMP:
		wsprintf(pszName, "Jump to position %d", param);
		break;

	case CMD_OFFSET:
		if (param)
			wsprintf(pszName, "Set Offset to %u", param << 8);
		else
			strcpy(s, "continue");
		break;

	case CMD_MIDI:
		if (param < 0x80)
		{
			wsprintf(pszName, "SFx macro: z=%02X (%d)", param, param);
		} else
		{
			wsprintf(pszName, "Fixed Macro Z%02X", param);
		}
		break;
	
	//rewbs.smoothVST
	case CMD_SMOOTHMIDI:
		if (param < 0x80)
		{
			wsprintf(pszName, "SFx macro: z=%02X (%d)", param, param);
		} else
		{
			wsprintf(pszName, "Fixed Macro Z%02X", param);
		}
		break;
	//end rewbs.smoothVST

	default:
		if (gFXInfo[ndx].dwParamMask == 0xF0)
		{
			// Sound control names
			if (((gFXInfo[ndx].dwEffect == CMD_XFINEPORTAUPDOWN) || (gFXInfo[ndx].dwEffect == CMD_S3MCMDEX))
			 && ((gFXInfo[ndx].dwParamValue & 0xF0) == 0x90) && ((param & 0xF0) == 0x90))
			{
				switch(param & 0x0F)
				{
				case 0x00:	strcpy(s, "90: Surround Off"); break;
				case 0x01:	strcpy(s, "91: Surround On"); break;
				case 0x08:	strcpy(s, "98: Reverb Off"); break;
				case 0x09:	strcpy(s, "99: Reverb On"); break;
				case 0x0A:	strcpy(s, "9A: Center surround"); break;
				case 0x0B:	strcpy(s, "9B: Quad surround"); break;
				case 0x0C:	strcpy(s, "9C: Global filters"); break;
				case 0x0D:	strcpy(s, "9D: Local filters"); break;
				case 0x0E:	strcpy(s, "9E: Play Forward"); break;
				case 0x0F:	strcpy(s, "9F: Play Backward"); break;
				default:	wsprintf(s, "%02X: undefined", param);
				}
			} else
			if (((gFXInfo[ndx].dwEffect == CMD_XFINEPORTAUPDOWN) || (gFXInfo[ndx].dwEffect == CMD_S3MCMDEX))
			 && ((gFXInfo[ndx].dwParamValue & 0xF0) == 0x70) && ((param & 0xF0) == 0x70))
			{
				switch(param & 0x0F)
				{
				case 0x00:	strcpy(s, "70: Past note cut"); break;
				case 0x01:	strcpy(s, "71: Past note off"); break;
				case 0x02:	strcpy(s, "72: Past note fade"); break;
				case 0x03:	strcpy(s, "73: NNA note cut"); break;
				case 0x04:	strcpy(s, "74: NNA continue"); break;
				case 0x05:	strcpy(s, "75: NNA note off"); break;
				case 0x06:	strcpy(s, "76: NNA note fade"); break;
				case 0x07:	strcpy(s, "77: Volume Env Off"); break;
				case 0x08:	strcpy(s, "78: Volume Env On"); break;
				case 0x09:	strcpy(s, "79: Pan Env Off"); break;
				case 0x0A:	strcpy(s, "7A: Pan Env On"); break;
				case 0x0B:	strcpy(s, "7B: Pitch Env Off"); break;
				case 0x0C:	strcpy(s, "7C: Pitch Env On"); break;
				default:	wsprintf(s, "%02X: undefined", param); break;
				}
			} else
			{
				wsprintf(s, "%d", param & 0x0F);
				if ((gFXInfo[ndx].dwEffect == CMD_S3MCMDEX) || (gFXInfo[ndx].dwEffect == CMD_MODCMDEX))
				{
					switch(param & 0xF0)
					{
					case 0x60:	if (gFXInfo[ndx].dwEffect == CMD_MODCMDEX) break;
					case 0xC0:
					case 0xD0:	strcat(s, " frames"); break;
					case 0xE0:	strcat(s, " rows"); break;
					case 0xF0:	wsprintf(s, "SF%X", param & 0x0F); break;
					}
				}
			}
			
		} else
		{
			wsprintf(s, "%d", param);
		}
	}
	strcat(pszName, s);
	return TRUE;
}


////////////////////////////////////////////////////////////////////////////////////////
// Volume column effects description

typedef struct MPTVOLCMDINFO
{
	DWORD dwVolCmd;		// VOLCMD_XXXX
	DWORD dwFormats;	// MOD_TYPE_XXX combo
	LPCSTR pszName;		// ie "Set Volume"
} MPTVOLCMDINFO;

#define MAX_VOLINFO		15			//rewbs.volOff & rewbs.velocity: increased from 13


const MPTVOLCMDINFO gVolCmdInfo[MAX_VOLINFO] =
{
	{VOLCMD_VOLUME,			MOD_TYPE_NOMOD,		"v: Set Volume"},
	{VOLCMD_PANNING,		MOD_TYPE_NOMOD,		"p: Set Panning"},
	{VOLCMD_VOLSLIDEUP,		MOD_TYPE_XMIT,		"c: Volume slide up"},
	{VOLCMD_VOLSLIDEDOWN,	MOD_TYPE_XMIT,		"d: Volume slide down"},
	{VOLCMD_FINEVOLUP,		MOD_TYPE_XMIT,		"a: Fine volume up"},
	{VOLCMD_FINEVOLDOWN,	MOD_TYPE_XMIT,		"b: Fine volume down"},
	{VOLCMD_VIBRATOSPEED,	MOD_TYPE_XMIT,		"u: Vibrato speed"},
	{VOLCMD_VIBRATO,		MOD_TYPE_XM,		"h: Vibrato depth"},
	{VOLCMD_PANSLIDELEFT,	MOD_TYPE_XM,		"l: Pan slide left"},
	{VOLCMD_PANSLIDERIGHT,	MOD_TYPE_XM,		"r: Pan slide right"},
	{VOLCMD_TONEPORTAMENTO,	MOD_TYPE_XMIT,		"g: Tone portamento"},
	{VOLCMD_PORTAUP,		MOD_TYPE_IT,		"f: Portamento up"},
	{VOLCMD_PORTADOWN,		MOD_TYPE_IT,		"e: Portamento down"},
	{VOLCMD_VELOCITY,		MOD_TYPE_IT,		":: velocity"},		//rewbs.velocity
	{VOLCMD_OFFSET,		MOD_TYPE_IT,		"o: offset"},		//rewbs.volOff
};


UINT CModDoc::GetNumVolCmds() const
//---------------------------------
{
	return MAX_VOLINFO;
}


LONG CModDoc::GetIndexFromVolCmd(UINT volcmd)
//-------------------------------------------
{
	for (UINT i=0; i<MAX_VOLINFO; i++)
	{
		if (gVolCmdInfo[i].dwVolCmd == volcmd) return i;
	}
	return -1;
}


UINT CModDoc::GetVolCmdFromIndex(UINT ndx)
//----------------------------------------
{
	return (ndx < MAX_VOLINFO) ? gVolCmdInfo[ndx].dwVolCmd : 0;
}


BOOL CModDoc::GetVolCmdInfo(UINT ndx, LPSTR s, DWORD *prangeMin, DWORD *prangeMax)
//--------------------------------------------------------------------------------
{
	if (s) s[0] = 0;
	if (prangeMin) *prangeMin = 0;
	if (prangeMax) *prangeMax = 0;
	if (ndx >= MAX_VOLINFO) return FALSE;
	if (s) strcpy(s, gVolCmdInfo[ndx].pszName);
	if ((prangeMin) && (prangeMax))
	{
		switch(gVolCmdInfo[ndx].dwVolCmd)
		{
		case VOLCMD_VOLUME:
			*prangeMax = 64;
			break;

		case VOLCMD_PANNING:
			*prangeMax = 64;
			if (m_SndFile.m_nType & MOD_TYPE_XM)
			{
				*prangeMin = 2;		// 0*4+2
				*prangeMax = 62;	// 15*4+2
			}
			break;

		default:
			*prangeMax = (m_SndFile.m_nType & MOD_TYPE_XM) ? 15 : 9;
		}
	}
	return (gVolCmdInfo[ndx].dwFormats & m_SndFile.m_nType) ? TRUE : FALSE;
}


void* CModDoc::GetChildFrame()
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	if (!pMainFrm) return 0;
	CMDIChildWnd *pMDIActive = pMainFrm->MDIGetActive();
	if (pMDIActive)
	{
		CView *pView = pMDIActive->GetActiveView();
		if ((pView) && (pView->GetDocument() == this))
			return ((CChildFrame *)pMDIActive);
	}
	POSITION pos = GetFirstViewPosition();
	while (pos != NULL)
	{
		CView *pView = GetNextView(pos);
		if ((pView) && (pView->GetDocument() == this))
			return (CChildFrame *)pView->GetParentFrame();
	}

	return NULL;
}

HWND CModDoc::GetEditPosition(UINT &row, UINT &pat, UINT &ord)
{
	HWND followSonghWnd;
	PATTERNVIEWSTATE *patternViewState;
	CChildFrame *pChildFrm = (CChildFrame *) GetChildFrame();

	if (strcmp("CViewPattern", pChildFrm->GetCurrentViewClassName()) == 0) // dirty HACK
	{
		followSonghWnd = pChildFrm->GetHwndView();
		patternViewState = new PATTERNVIEWSTATE;
		pChildFrm->SendViewMessage(VIEWMSG_SAVESTATE, (LPARAM)patternViewState);
		
		pat = patternViewState->nPattern;
		row = patternViewState->nRow;
		ord = patternViewState->nOrder;
		
		delete patternViewState;
	}
	else	//patern editor object does not exist (i.e. is not active)  - use saved state.
	{
		followSonghWnd = NULL;
		patternViewState = pChildFrm->GetPatternViewState();

		pat = patternViewState->nPattern;
		row = patternViewState->nRow;
		ord = patternViewState->nOrder;
	}

	return followSonghWnd;

}


void CModDoc::OnPatternRestart()
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	CChildFrame *pChildFrm = (CChildFrame *) GetChildFrame();

	if ((pMainFrm) && (pChildFrm))
	{
		CSoundFile *pSndFile = GetSoundFile();
		UINT nPat,nOrd,nRow;
		HWND followSonghWnd;

		followSonghWnd = GetEditPosition(nRow, nPat, nOrd);
		
		BEGIN_CRITICAL();
		// Cut instruments/samples
		for (UINT i=0; i<MAX_CHANNELS; i++)
		{
			pSndFile->Chn[i].nPatternLoopCount = 0;
			pSndFile->Chn[i].nPatternLoop = 0;
			pSndFile->Chn[i].nFadeOutVol = 0;
			pSndFile->Chn[i].dwFlags |= CHN_NOTEFADE | CHN_KEYOFF;
		}
		if ((nOrd < MAX_PATTERNS) && (pSndFile->Order[nOrd] == nPat)) pSndFile->m_nCurrentPattern = pSndFile->m_nNextPattern = nOrd;
		pSndFile->m_dwSongFlags &= ~(SONG_PAUSED|SONG_STEP);
		pSndFile->LoopPattern(nPat);
		pSndFile->m_nNextRow = 0;
		pSndFile->ResetTotalTickCount();
		END_CRITICAL();

		pMainFrm->ResetElapsedTime();
		if (pMainFrm->GetModPlaying() != this)
		{
			pSndFile->ResumePlugins();
			pMainFrm->PlayMod(this, followSonghWnd, MPTNOTIFY_POSITION);
		}
		else
		{
			pSndFile->SuspendPlugins();
			pSndFile->ResumePlugins();
		}
	}
	//SwitchToView();
}

void CModDoc::OnPatternPlay()
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	CChildFrame *pChildFrm = (CChildFrame *) GetChildFrame();

	if ((pMainFrm) && (pChildFrm))
	{
		CSoundFile *pSndFile = GetSoundFile();
		UINT nRow,nPat,nOrd;
		HWND followSonghWnd;

		followSonghWnd = GetEditPosition(nRow,nPat,nOrd);
	
		BEGIN_CRITICAL();
		// Cut instruments/samples
		for (UINT i=pSndFile->m_nChannels; i<MAX_CHANNELS; i++)
		{
			pSndFile->Chn[i].dwFlags |= CHN_NOTEFADE | CHN_KEYOFF;
		}
		pSndFile->m_dwSongFlags &= ~(SONG_PAUSED|SONG_STEP);
		pSndFile->LoopPattern(nPat);
		pSndFile->m_nNextRow = nRow;
		END_CRITICAL();

		pMainFrm->ResetElapsedTime();
		if (pMainFrm->GetModPlaying() != this)
		{
			pSndFile->ResumePlugins();
			pMainFrm->PlayMod(this, followSonghWnd, MPTNOTIFY_POSITION);
		}
		else
		{
			pSndFile->SuspendPlugins();
			pSndFile->ResumePlugins();
		}
		
	}
	//SwitchToView();

}

void CModDoc::OnPatternPlayNoLoop()
{
	CMainFrame *pMainFrm = CMainFrame::GetMainFrame();
	CChildFrame *pChildFrm = (CChildFrame *) GetChildFrame();

	if ((pMainFrm) && (pChildFrm))
	{
		CSoundFile *pSndFile = GetSoundFile();
		UINT nPat,nOrd,nRow;
		HWND followSonghWnd;

		followSonghWnd = GetEditPosition(nRow,nPat,nOrd);

		BEGIN_CRITICAL();
		// Cut instruments/samples
		for (UINT i=pSndFile->m_nChannels; i<MAX_CHANNELS; i++)
		{
			pSndFile->Chn[i].dwFlags |= CHN_NOTEFADE | CHN_KEYOFF;
		}
		pSndFile->m_dwSongFlags &= ~(SONG_PAUSED|SONG_STEP);
		pSndFile->SetCurrentOrder(nOrd);
		if (pSndFile->Order[nOrd] == nPat)
			pSndFile->DontLoopPattern(nPat, nRow);
		else
			pSndFile->LoopPattern(nPat);
		pSndFile->m_nNextRow = nRow;
		END_CRITICAL();

		pMainFrm->ResetElapsedTime();
		
		if (pMainFrm->GetModPlaying() != this)
		{
			pSndFile->ResumePlugins();
			pMainFrm->PlayMod(this, followSonghWnd, MPTNOTIFY_POSITION);
		}
		else
		{
			pSndFile->SuspendPlugins();
			pSndFile->ResumePlugins();
		}
	}
	//SwitchToView();
}

LRESULT CModDoc::OnCustomKeyMsg(WPARAM wParam, LPARAM lParam)
{
	if (wParam == kcNull)
		return NULL;

	switch(wParam)
	{
		case kcViewGeneral: OnEditGlobals(); break;
		case kcViewPattern: OnEditPatterns(); break;
		case kcViewSamples: OnEditSamples(); break;
		case kcViewInstruments: OnEditInstruments(); break;
		case kcViewComments: OnEditComments(); break;

		case kcFileSaveAsWave:	OnFileWaveConvert(); break;
		case kcFileSaveAsMP3:	OnFileMP3Convert(); break;
		case kcFileSaveMidi: OnFileMidiConvert(); break;
		case kcEstimateSongLength: OnEstimateSongLength(); break;
		case kcFileSave:	DoSave(m_strPathName, 0); break;
		case kcFileSaveAs:	DoSave(NULL, 1); break;
		case kcFileClose:	OnCloseDocument(); break;

		case kcPlayPatternFromCursor: OnPatternPlay(); break;
		case kcPlayPatternFromStart: OnPatternRestart(); break;
		case kcPlaySongFromCursor: OnPatternPlayNoLoop(); break;
		case kcPlaySongFromStart: OnPlayerPlayFromStart(); break;
		case kcPlayPauseSong: OnPlayerPlay(); break;
		case kcStopSong: OnPlayerStop(); break;
//		case kcPauseSong: OnPlayerPause(); break;
			

	}

	return wParam;
}


void CModDoc::TogglePluginEditor(UINT m_nCurrentPlugin)
{
	PSNDMIXPLUGIN pPlugin;

	pPlugin = &m_SndFile.m_MixPlugins[m_nCurrentPlugin];
	if (pPlugin && pPlugin->pMixPlugin)
	{
		CVstPlugin *pVstPlugin = (CVstPlugin *)pPlugin->pMixPlugin;
//		if (pVstPlugin->HasEditor())
//		{
			pVstPlugin->ToggleEditor();
/*		} else
		{
			UINT nCommands = pVstPlugin->GetNumCommands();
			if (nCommands > 10) nCommands = 10;
			if (nCommands)
			{
				CHAR s[32];
				HMENU hMenu = ::CreatePopupMenu();
				
				if (!hMenu)	return;
				for (UINT i=0; i<nCommands; i++)
				{
					s[0] = 0;
					pVstPlugin->GetCommandName(i, s);
					if (s[0])
					{
						::AppendMenu(hMenu, MF_STRING, ID_FXCOMMANDS_BASE+i, s);
					}
				}
				CPoint pt;
				GetCursorPos(&pt);
				::TrackPopupMenu(hMenu, TPM_LEFTALIGN|TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWnd, NULL);
				::DestroyMenu(hMenu);
			}

		}
*/
	}

	return;
}