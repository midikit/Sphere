#include "graysvr.h"	// predef header.
#include "../network/network.h"

extern "C"
{
	void globalstartsymbol() { }	// put this here as just the starting offset
	const int globalstartdata = 0xFFFFFFFF;
}

//*********************************************************
// CAccounts

void CAccounts::Account_Add(CAccount *pAccount)
{
	ADDTOCALLSTACK("CAccounts::Account_Add");
	ASSERT(pAccount);

	if ( !g_Serv.IsLoading() )
	{
		CScriptTriggerArgs Args;
		Args.Init(pAccount->GetName());
		TRIGRET_TYPE tr = TRIGRET_RET_FALSE;

		g_Serv.r_Call("f_onaccount_create", &g_Serv, &Args, NULL, &tr);
		if ( tr == TRIGRET_RET_TRUE )
		{
			g_Log.Event(LOGL_ERROR|LOGM_INIT, "Account '%s': creation blocked via script\n", pAccount->GetName());
			Account_Delete(pAccount);
			return;
		}
	}
	m_Accounts.AddSortKey(pAccount, pAccount->GetName());
}

bool CAccounts::Account_Delete(CAccount *pAccount)
{
	ADDTOCALLSTACK("CAccounts::Account_Delete");
	ASSERT(pAccount);

	CScriptTriggerArgs Args;
	Args.Init(pAccount->GetName());
	TRIGRET_TYPE tr = TRIGRET_RET_FALSE;

	g_Serv.r_Call("f_onaccount_delete", &g_Serv, &Args, NULL, &tr);
	if ( tr == TRIGRET_RET_TRUE )
		return false;

	m_Accounts.DeleteOb(pAccount);
	return true;
}

bool CAccounts::Account_SaveAll()
{
	ADDTOCALLSTACK("CAccounts::Account_SaveAll");
	EXC_TRY("SaveAll");
	LPCTSTR pszBaseDir = g_Cfg.m_sAcctBaseDir.IsEmpty() ? g_Cfg.m_sWorldBaseDir : g_Cfg.m_sAcctBaseDir;

	// Look for changes first
	Account_LoadAll(true);

	CScript s;
	if ( !CWorld::OpenScriptBackup(s, pszBaseDir, "accu", g_World.m_iSaveCountID) )
		return false;

	s.Printf("// " SPHERE_TITLE " accounts file.\n"
			 "// NOTE: This file cannot be edited while the server is running.\n"
			 "// Changes must be made on " SPHERE_FILE "acct" SPHERE_SCRIPT " to be applied on next worldsave.\n");

	for ( size_t i = 0; i < m_Accounts.GetCount(); i++ )
	{
		CAccountRef pAccount = Account_Get(i);
		if ( pAccount )
			pAccount->r_Write(s);
	}

	s.WriteSection("EOF");

	Account_LoadAll(true, true);	// clear the changes file
	return true;
	EXC_CATCH;
	return false;
}

bool CAccounts::Account_Load(LPCTSTR pszName, CScript &s, bool fChanges)
{
	ADDTOCALLSTACK("CAccounts::Account_Load");

	// Only read as "[ACCOUNT name]" format if arguments exist
	if ( s.HasArgs() && !strcmpi(pszName, "ACCOUNT") )
		pszName = s.GetArgStr();

	TCHAR szName[MAX_ACCOUNT_NAME_SIZE];
	if ( !CAccount::NameStrip(szName, pszName) )
	{
		if ( !fChanges )
		{
			g_Log.Event(LOGL_ERROR|LOGM_INIT, "Account '%s': bad name\n", pszName);
			return false;
		}
	}

	m_fLoading = true;
	CAccountRef pAccount = Account_Find(szName);
	if ( pAccount )
	{
		if ( !fChanges )
		{
			g_Log.Event(LOGL_ERROR|LOGM_INIT, "Account '%s': duplicated name\n", pszName);
			return false;
		}
	}
	else
	{
		pAccount = new CAccount(szName);
		ASSERT(pAccount);
	}
	pAccount->r_Load(s);
	m_fLoading = false;

	return true;
}

bool CAccounts::Account_LoadAll(bool fChanges, bool fClearChanges)
{
	ADDTOCALLSTACK("CAccounts::Account_LoadAll");
	LPCTSTR pszBaseDir = g_Cfg.m_sAcctBaseDir.IsEmpty() ? g_Cfg.m_sWorldBaseDir : g_Cfg.m_sAcctBaseDir;
	LPCTSTR pszBaseName = fChanges ? SPHERE_FILE "acct" : SPHERE_FILE "accu";

	char *z = Str_GetTemp();
	strncpy(z, pszBaseDir, _MAX_PATH);
	strncat(z, pszBaseName, _MAX_PATH);

	if ( !fChanges )
		g_Log.Event(LOGM_INIT, "Loading %s%s\n", z, SPHERE_SCRIPT);

	CScript s;
	if ( !s.Open(z, OF_READ|OF_TEXT|OF_DEFAULTMODE|(fChanges ? OF_NONCRIT : 0)) )
	{
		if ( !fChanges )
		{
			if ( Account_LoadAll(true) )	// if we have changes then we are ok.
				return true;
			if ( Account_SaveAll() )	// auto-create account files
				return true;
			g_Log.Event(LOGL_FATAL|LOGM_INIT, "Can't open account file '%s'\n", static_cast<LPCTSTR>(s.GetFilePath()));
			return false;
		}
	}

	if ( fClearChanges )
	{
		// Empty the changes file
		ASSERT(fChanges);
		s.Close();
		if ( !s.Open(NULL, OF_WRITE|OF_TEXT|OF_DEFAULTMODE) )
			g_Log.Event(LOGL_FATAL|LOGM_INIT, "Can't open account file '%s'\n", static_cast<LPCTSTR>(s.GetFilePath()));
		else
			s.WriteString("// " SPHERE_TITLE " accounts update file.\n"
						  "// Account changes should be made here and will be applied on next worldsave.\n"
						  "// Use 'ACCOUNT UPDATE' command to force the update immediately.\n\n");
		return true;
	}

	CScriptFileContext ScriptContext(&s);
	while ( s.FindNextSection() )
		Account_Load(s.GetKey(), s, fChanges);

	if ( !fChanges )
		Account_LoadAll(true);

	return true;
}

enum VACS_TYPE
{
	VACS_ADD,		// Add new account
	VACS_ADDMD5,	// Add new account using MD5 password
	VACS_BLOCKED,	// Block the account
	VACS_HELP,		// Show account commands
	VACS_JAILED,	// Jail the account
	VACS_UNUSED,	// Call an function on unused account
	VACS_UPDATE,	// Process the acct file to update accounts
	VACS_QTY
};

LPCTSTR const CAccounts::sm_szVerbKeys[] =
{
	"ADD",
	"ADDMD5",
	"BLOCKED",
	"HELP",
	"JAILED",
	"UNUSED",
	"UPDATE",
	NULL,
};

bool CAccounts::Account_OnCmd(TCHAR *pszArgs, CTextConsole *pSrc)
{
	ADDTOCALLSTACK("CAccounts::Account_OnCmd");
	ASSERT(pSrc);

	if ( pSrc->GetPrivLevel() < PLEVEL_Admin )
		return false;

	TCHAR *ppCmd[5];
	size_t iQty = Str_ParseCmds(pszArgs, ppCmd, COUNTOF(ppCmd));

	int index = VACS_HELP;
	if ( iQty >= 1 )
		index = FindTableSorted(ppCmd[0], sm_szVerbKeys, COUNTOF(sm_szVerbKeys) - 1);

	switch ( static_cast<VACS_TYPE>(index) )
	{
		case VACS_ADD:
			return Cmd_AddNew(pSrc, ppCmd[1], ppCmd[2]);
		case VACS_ADDMD5:
			return Cmd_AddNew(pSrc, ppCmd[1], ppCmd[2], true);
		case VACS_BLOCKED:
			return Cmd_ListUnused(pSrc, ppCmd[1], ppCmd[2], ppCmd[3], PRIV_BLOCKED);
		case VACS_HELP:
			pSrc->SysMessagef(
				"Available commands:\n"
				"ACCOUNT ADD [login] [password]\n"
				"ACCOUNT ADDMD5 [login] [password_hash]\n"
				"ACCOUNT UNUSED [days] [command]\n"
				"ACCOUNT UPDATE\n"
				"ACCOUNT [login] BLOCK [0/1]\n"
				"ACCOUNT [login] DELETE\n"
				"ACCOUNT [login] JAIL [0/1]\n"
				"ACCOUNT [login] MD5PASSWORD [password_hash]\n"
				"ACCOUNT [login] PASSWORD [password]\n"
				"ACCOUNT [login] PLEVEL [0-7]\n"
			);
			return true;
		case VACS_JAILED:
			return Cmd_ListUnused(pSrc, ppCmd[1], ppCmd[2], ppCmd[3], PRIV_JAILED);
		case VACS_UNUSED:
			return Cmd_ListUnused(pSrc, ppCmd[1], ppCmd[2], ppCmd[3]);
		case VACS_UPDATE:
			Account_SaveAll();
			return true;
		default:
			break;
	}

	CAccountRef pAccount = Account_Find(ppCmd[0]);
	if ( !pAccount )
	{
		pSrc->SysMessagef("Account '%s' does not exist\n", ppCmd[0]);
		return false;
	}

	if ( !ppCmd[1] || !ppCmd[1][0] )
	{
		CClient *pClient = pAccount->FindClient();
		char *z = Str_GetTemp();
		sprintf(z, "Account '%s': PLEVEL:%d, BLOCK:%d, IP:%s, CONNECTED:%s, ONLINE:%s\n", pAccount->GetName(), pAccount->GetPrivLevel(), static_cast<int>(pAccount->IsPriv(PRIV_BLOCKED)), pAccount->m_Last_IP.GetAddrStr(), pAccount->m_dateLastConnect.Format(NULL), (pClient ? (pClient->GetChar() ? pClient->GetChar()->GetName() : "<not logged>") : "no"));
		pSrc->SysMessage(z);
		return true;
	}
	else
	{
		CGString sVal;
		if ( ppCmd[4] && ppCmd[4][0] )
			sVal.Format("%s %s %s", ppCmd[2], ppCmd[3], ppCmd[4]);
		else if ( ppCmd[3] && ppCmd[3][0] )
			sVal.Format("%s %s", ppCmd[2], ppCmd[3]);
		else if ( ppCmd[2] && ppCmd[2][0] )
			sVal.Format("%s", ppCmd[2]);

		CScript script(ppCmd[1], sVal.GetPtr());
		return pAccount->r_Verb(script, pSrc);
	}
}

CAccountRef CAccounts::Account_Get(size_t index)
{
	ADDTOCALLSTACK("CAccounts::Account_Get");
	if ( m_Accounts.IsValidIndex(index) )
		return static_cast<CAccountRef>(m_Accounts[index]);
	return NULL;
}

CAccountRef CAccounts::Account_Find(LPCTSTR pszName)
{
	ADDTOCALLSTACK("CAccounts::Account_Find");

	TCHAR szName[MAX_ACCOUNT_NAME_SIZE];
	if ( !CAccount::NameStrip(szName, pszName) )
		return NULL;

	size_t i = m_Accounts.FindKey(szName);
	if ( i != m_Accounts.BadIndex() )
		return Account_Get(i);

	return NULL;
}

CAccountRef CAccounts::Account_FindCreate(LPCTSTR pszName, bool fCreate)
{
	ADDTOCALLSTACK("CAccounts::Account_FindCreate");

	CAccountRef pAccount = Account_Find(pszName);
	if ( pAccount )
		return pAccount;

	// Account not found, check if it should be created
	if ( fCreate )
	{
		TCHAR szName[MAX_ACCOUNT_NAME_SIZE];
		if ( CAccount::NameStrip(szName, pszName) )
		{
			pAccount = new CAccount(szName);
			ASSERT(pAccount);
			if ( (g_Serv.m_eAccApp == ACCAPP_GuestAuto) || (g_Serv.m_eAccApp == ACCAPP_GuestTrial) )
				pAccount->SetPrivLevel(PLEVEL_Guest);
			return pAccount;
		}
		g_Log.Event(LOGL_ERROR|LOGM_INIT, "Account '%s': bad name\n", pszName);
	}
	return NULL;
}

CAccountRef CAccounts::Account_FindChat(LPCTSTR pszName)
{
	ADDTOCALLSTACK("CAccounts::Account_FindChat");
	for ( size_t i = 0; i < m_Accounts.GetCount(); i++ )
	{
		CAccountRef pAccount = Account_Get(i);
		if ( pAccount && (pAccount->m_sChatName.CompareNoCase(pszName) == 0) )
			return pAccount;
	}
	return NULL;
}

bool CAccounts::Cmd_AddNew(CTextConsole *pSrc, LPCTSTR pszName, LPCTSTR pszPassword, bool fMD5)
{
	ADDTOCALLSTACK("CAccounts::Cmd_AddNew");
	if ( !pszName )
	{
		g_Log.Event(LOGL_ERROR|LOGM_INIT, "Username is required to add an account\n");
		return false;
	}

	CAccountRef pAccount = Account_Find(pszName);
	if ( pAccount )
	{
		pSrc->SysMessagef("Account '%s' already exists\n", pszName);
		return false;
	}

	TCHAR szName[MAX_ACCOUNT_NAME_SIZE];
	if ( !CAccount::NameStrip(szName, pszName) )
	{
		g_Log.Event(LOGL_ERROR|LOGM_INIT, "Account '%s': bad name\n", pszName);
		return false;
	}

	pAccount = new CAccount(szName);
	ASSERT(pAccount);
	pAccount->SetPassword(pszPassword, fMD5);
	pAccount->m_dateFirstConnect = pAccount->m_dateLastConnect = CGTime::GetCurrentTime();
	return true;
}

bool CAccounts::Cmd_ListUnused(CTextConsole *pSrc, LPCTSTR pszDays, LPCTSTR pszVerb, LPCTSTR pszArgs, WORD wPrivFlags)
{
	ADDTOCALLSTACK("CAccounts::Cmd_ListUnused");
	if ( !pszVerb )
	{
		pszVerb = "SHOW";
		pszArgs = "LASTCONNECTDATE";
	}

	int iDaysTest = Exp_GetVal(pszDays);
	bool fDelete = !strcmpi(pszVerb, "DELETE");

	CGTime datetime = CGTime::GetCurrentTime();
	int iDaysCur = datetime.GetDaysTotal();

	size_t iCountOrig = Account_GetCount();
	size_t iCountCheck = iCountOrig;
	size_t iCount = 0;
	for ( size_t i = 0; ; i++ )
	{
		if ( Account_GetCount() < iCountCheck )
		{
			iCountCheck--;
			i--;
		}

		CAccountRef pAccount = Account_Get(i);
		if ( !pAccount )
			break;

		int iDaysAcc = pAccount->m_dateLastConnect.GetDaysTotal();
		if ( !iDaysAcc )
			iDaysAcc = pAccount->m_dateFirstConnect.GetDaysTotal();		// use creation date if the account was never used
		if ( iDaysCur - iDaysAcc < iDaysTest )
			continue;

		if ( wPrivFlags && !pAccount->IsPriv(wPrivFlags) )
			continue;

		iCount++;
		if ( fDelete && (pAccount->GetPrivLevel() > PLEVEL_Player) )
		{
			iCount--;
			pSrc->SysMessagef("Can't delete PrivLevel %d account '%s' this way\n", pAccount->GetPrivLevel(), pAccount->GetName());
		}
		else
		{
			CScript script(pszVerb, pszArgs);
			pAccount->r_Verb(script, pSrc);
		}
	}

	pSrc->SysMessagef("Matched %" FMTSIZE_T " of %" FMTSIZE_T " accounts unused for %d days\n", iCount, iCountOrig, iDaysTest);

	if ( fDelete )
	{
		size_t iDeleted = iCountOrig - Account_GetCount();
		if ( iDeleted < iCount )
			pSrc->SysMessagef("%" FMTSIZE_T " deleted, %" FMTSIZE_T " cleared of characters (must try to delete again)\n", iDeleted, iCount - iDeleted);
		else if ( iDeleted > 0 )
			pSrc->SysMessagef("All %" FMTSIZE_T " unused accounts deleted\n", iDeleted);
		else
			pSrc->SysMessagef("No accounts deleted\n");
	}

	return true;
}

//*********************************************************
// CAccount

CAccount::CAccount(LPCTSTR pszName)
{
	g_Serv.StatInc(SERV_STAT_ACCOUNTS);

	TCHAR szName[MAX_ACCOUNT_NAME_SIZE];
	if ( !CAccount::NameStrip(szName, pszName) )
		g_Log.Event(LOGL_ERROR|LOGM_INIT, "Account '%s': bad name\n", pszName);

	m_sName = szName;
	m_PrivLevel = PLEVEL_Player;
	m_PrivFlags = 0;
	m_MaxChars = 0;
	m_Total_Connect_Time = 0;
	m_Last_Connect_Time = 0;

	g_Accounts.Account_Add(this);
}

CAccount::~CAccount()
{
	g_Serv.StatDec(SERV_STAT_ACCOUNTS);
	DeleteChars();
}

bool CAccount::NameStrip(TCHAR *pszNameOut, LPCTSTR pszNameIn)
{
	ADDTOCALLSTACK("CAccount::NameStrip");

	if ( Str_GetBare(pszNameOut, pszNameIn, MAX_ACCOUNT_NAME_SIZE, ACCOUNT_NAME_VALID_CHAR) <= 0 )		// check length
		return false;
	if ( IsStrNumeric(pszNameOut) )		// check numeric characters
		return false;
	if ( strchr(pszNameOut, 0xA) || strchr(pszNameOut, 0xC) || strchr(pszNameOut, 0xD) )		// check newline characters
		return false;
	if ( !strcmpi(pszNameOut, "EOF") || !strcmpi(pszNameOut, "ACCOUNT") )	// check invalid names
		return false;
	if ( FindTableSorted(pszNameOut, CAccounts::sm_szVerbKeys, COUNTOF(CAccounts::sm_szVerbKeys) - 1) >= 0 )	// check invalid keywords
		return false;
	if ( g_Cfg.IsObscene(pszNameOut) )
		return false;

	return true;
}

bool CAccount::SetPassword(LPCTSTR pszPassword, bool fMD5)
{
	ADDTOCALLSTACK("CAccount::SetPassword");

	if ( Str_Check(pszPassword) )	// prevents exploits
		return false;

	if ( !g_Serv.IsLoading() )
	{
		CScriptTriggerArgs Args;
		Args.Init(GetName());
		Args.m_VarsLocal.SetStrNew("Password", pszPassword);
		TRIGRET_TYPE tr = TRIGRET_RET_FALSE;

		g_Serv.r_Call("f_onaccount_pwchange", &g_Serv, &Args, NULL, &tr);
		if ( tr == TRIGRET_RET_TRUE )
			return false;
	}

	size_t iGivenPasswordLength = strlen(pszPassword);

	if ( g_Cfg.m_fMd5Passwords && fMD5 )
	{
		// If MD5 is enabled and the password is already given in MD5 hash, just set it directly
		if ( iGivenPasswordLength == 32 )
			m_sPassword = pszPassword;
		return true;
	}

	size_t iCurrentPasswordBufferSize = minimum(MAX_ACCOUNT_PASSWORD_ENTER, iGivenPasswordLength) + 1;
	char *currentPassword = new char[iCurrentPasswordBufferSize];
	strcpylen(currentPassword, pszPassword, iCurrentPasswordBufferSize);

	if ( g_Cfg.m_fMd5Passwords )
	{
		char digest[33];
		if ( !g_Accounts.m_fLoading )
		{
			// Auto-hash if not loading
			CMD5::fastDigest(digest, currentPassword);
			m_sPassword = digest;
		}
		else
		{
			if ( iGivenPasswordLength == 32 )
			{
				// Password already in MD5 hash, just set it directly
				m_sPassword = pszPassword;
			}
			else
			{
				// Password must be converted to MD5 hash
				g_Log.Event(LOGM_INIT|LOGL_EVENT, "Account '%s': password converted to MD5 hash\n", GetName());
				CMD5::fastDigest(digest, currentPassword);
				m_sPassword = digest;
			}
		}
	}
	else
		m_sPassword = currentPassword;

	delete[] currentPassword;
	return true;
}

void CAccount::SetNewPassword(LPCTSTR pszPassword)
{
	ADDTOCALLSTACK("CAccount::SetNewPassword");

	if ( !pszPassword || !pszPassword[0] )	// no password given, so auto-generate it
	{
		static TCHAR const szValidChars[] = "ABCDEFGHJKLMNPQRTUVWXYZ2346789";
		size_t iLenArray = strlen(szValidChars);
		size_t iLen = static_cast<size_t>(Calc_GetRandVal2(MAX_ACCOUNT_PASSWORD_ENTER / 2, MAX_ACCOUNT_PASSWORD_ENTER));

		TCHAR szTmp[MAX_ACCOUNT_PASSWORD_ENTER + 1];
		for ( size_t i = 0; i < iLen; ++i )
			szTmp[i] = szValidChars[Calc_GetRandVal(iLenArray)];

		szTmp[iLen] = '\0';
		m_sNewPassword = szTmp;
		return;
	}

	m_sNewPassword = pszPassword;
	if ( m_sNewPassword.GetLength() > MAX_ACCOUNT_PASSWORD_ENTER )
		m_sNewPassword.SetLength(MAX_ACCOUNT_PASSWORD_ENTER);
}

bool CAccount::CheckPassword(LPCTSTR pszPassword)
{
	ADDTOCALLSTACK("CAccount::CheckPassword");
	ASSERT(pszPassword);

	if ( m_sPassword.IsEmpty() )
	{
		// If account password is empty, set the password given by the client trying to connect
		if ( !SetPassword(pszPassword) )
			return false;
	}

	CScriptTriggerArgs Args;
	Args.m_VarsLocal.SetStrNew("Account", GetName());
	Args.m_VarsLocal.SetStrNew("Password", pszPassword);
	TRIGRET_TYPE tr = TRIGRET_RET_FALSE;

	g_Serv.r_Call("f_onaccount_connect", &g_Serv, &Args, NULL, &tr);
	if ( tr == TRIGRET_RET_TRUE )
		return false;
	if ( tr == TRIGRET_RET_HALFBAKED )
		return true;

	if ( g_Cfg.m_fMd5Passwords )
	{
		char digest[33];
		CMD5::fastDigest(digest, pszPassword);
		pszPassword = static_cast<LPCTSTR>(digest);
	}

	// Check password
	if ( !strcmpi(pszPassword, GetPassword()) )
		return true;

	if ( !m_sNewPassword.IsEmpty() && !strcmpi(pszPassword, GetNewPassword()) )
	{
		// Using new password, so set it as default
		SetPassword(m_sNewPassword);
		m_sNewPassword.Empty();
		return true;
	}

	return false;
}

bool CAccount::CheckPasswordTries(CSocketAddress csaPeerName)
{
	ADDTOCALLSTACK("CAccount::CheckPasswordTries");
	bool fReturn = true;
	DWORD dwCurrentIP = csaPeerName.GetAddrIP();
	UINT64 uTimeCurrent = CServTime::GetCurrentTime().GetTimeRaw();

	BlockLocalTime_t::iterator itData = m_BlockIP.find(dwCurrentIP);
	if ( itData != m_BlockIP.end() )
	{
		BlockLocalTimePair_t itResult = (*itData).second;
		TimeTriesStruct_t &ttsData = itResult.first;

		if ( ttsData.m_BlockDelay > uTimeCurrent )
			return false;

		if ( uTimeCurrent - ttsData.m_LastTry > 60 * TICK_PER_SEC )
		{
			// Reset counter after wait 60s from last try
			ttsData.m_BlockDelay = 0;
			itResult.second = 0;
		}
		else
		{
			itResult.second++;
			if ( itResult.second >= g_Cfg.m_iClientLoginMaxTries )
			{
				// Max tries reached, apply temporary ban
				ttsData.m_BlockDelay = uTimeCurrent + static_cast<UINT64>(g_Cfg.m_iClientLoginTempBan);
				fReturn = false;
			}
		}
		ttsData.m_LastTry = uTimeCurrent;
		m_BlockIP[dwCurrentIP] = itResult;
	}
	else
	{
		TimeTriesStruct_t ttsData;
		ttsData.m_LastTry = uTimeCurrent;
		ttsData.m_BlockDelay = 0;
		m_BlockIP[dwCurrentIP] = std::make_pair(ttsData, 0);
	}

	if ( m_BlockIP.size() > 100 )
		ClearPasswordTries();

	return fReturn;
}

void CAccount::ClearPasswordTries()
{
	ADDTOCALLSTACK("CAccount::ClearPasswordTries");
	UINT64 uTimeCurrent = CServTime::GetCurrentTime().GetTimeRaw();
	for ( BlockLocalTime_t::iterator itData = m_BlockIP.begin(); itData != m_BlockIP.end(); )
	{
		BlockLocalTimePair_t itResult = (*itData).second;
		if ( uTimeCurrent - itResult.first.m_LastTry > 60 * TICK_PER_SEC )
		{
#ifdef _WIN32
			itData = m_BlockIP.erase(itData);
#else
			BlockLocalTime_t::iterator itDelete = itData;
			++itData;
			m_BlockIP.erase(itDelete);
#endif
		}
		else
			++itData;
	}
}

static LPCTSTR const sm_szPrivLevels[PLEVEL_QTY + 1] =
{
	"Guest",		// 0 = This is just a guest account (cannot PK)
	"Player",		// 1 = Player or NPC
	"Counsel",		// 2 = Can travel and give advice
	"Seer",			// 3 = Can make things and NPC's but not directly bother players
	"GM",			// 4 = GM command clearance
	"Dev",			// 5 = Not bothererd by GM's
	"Admin",		// 6 = Can switch in/out GM mode and assign GM's
	"Owner",		// 7 = Highest level allowed
	NULL
};

PLEVEL_TYPE CAccount::GetPrivLevelText(LPCTSTR pszFlags)	// static
{
	ADDTOCALLSTACK("CAccount::GetPrivLevelText");
	int plevel = FindTable(pszFlags, sm_szPrivLevels, COUNTOF(sm_szPrivLevels) - 1);
	if ( plevel >= 0 )
		return static_cast<PLEVEL_TYPE>(plevel);

	plevel = Exp_GetVal(pszFlags);
	if ( plevel < PLEVEL_Guest )
		return PLEVEL_Guest;
	if ( plevel > PLEVEL_Owner )
		return PLEVEL_Owner;
	return static_cast<PLEVEL_TYPE>(plevel);
}

bool CAccount::SetAutoResDisp(CClient *pClient)
{
	ADDTOCALLSTACK("CAccount::SetAutoResDisp");
	// Set account RESDISP automatically based on player's client version
	if ( !pClient )
		return false;

	if ( pClient->m_NetState->isClientVersion(MINCLIVER_TOL) )
		return SetResDisp(RDS_TOL);
	else if ( pClient->m_NetState->isClientVersion(MINCLIVER_HS) )
		return SetResDisp(RDS_HS);
	else if ( pClient->m_NetState->isClientVersion(MINCLIVER_SA) )
		return SetResDisp(RDS_SA);
	else if ( pClient->m_NetState->isClientVersion(MINCLIVER_ML) )
		return SetResDisp(RDS_ML);
	else if ( pClient->m_NetState->isClientVersion(MINCLIVER_SE) )
		return SetResDisp(RDS_SE);
	else if ( pClient->m_NetState->isClientVersion(MINCLIVER_AOS) )
		return SetResDisp(RDS_AOS);
	else if ( pClient->m_NetState->isClientVersion(MINCLIVER_LBR) )
		return SetResDisp(RDS_LBR);
	else if ( pClient->m_NetState->isClientVersion(MINCLIVER_T2A) )
		return SetResDisp(RDS_T2A);
	else
		return SetResDisp(RDS_NONE);
}

void CAccount::OnLogin(CClient *pClient)
{
	ADDTOCALLSTACK("CAccount::OnLogin");
	ASSERT(pClient);

	pClient->m_timeLogin = CServTime::GetCurrentTime();

	if ( pClient->GetConnectType() == CONNECT_TELNET )
		g_Serv.m_iAdminClients++;
	if ( GetPrivLevel() >= PLEVEL_Counsel )
		m_PrivFlags |= PRIV_GM_PAGE;

	CGTime datetime = CGTime::GetCurrentTime();		// real world date/time

	if ( !m_Total_Connect_Time )
	{
		m_First_IP = pClient->GetPeer();
		m_dateFirstConnect = datetime;
	}

	m_Last_IP = pClient->GetPeer();
	//m_dateLastConnect = datetime;
	//m_TagDefs.SetStr("LastLogged", false, m_dateLastConnect.Format(NULL));

	g_Log.Event(LOGM_CLIENTS_LOG, "%lx:Login '%s'\n", pClient->GetSocketID(), GetName());
}

void CAccount::OnLogout(CClient *pClient, bool fWasChar)
{
	ADDTOCALLSTACK("CAccount::OnLogout");
	ASSERT(pClient);

	if ( pClient->GetConnectType() == CONNECT_TELNET )
		g_Serv.m_iAdminClients--;

	if ( pClient->IsConnectTypePacket() && fWasChar )
	{
		m_Last_Connect_Time = -g_World.GetTimeDiff(pClient->m_timeLogin) / (TICK_PER_SEC * 60);
		if ( m_Last_Connect_Time < 0 )
			m_Last_Connect_Time = 0;
		m_Total_Connect_Time += m_Last_Connect_Time;
	}
}

bool CAccount::Kick(CTextConsole *pSrc, bool fBlock)
{
	ADDTOCALLSTACK("CAccount::Kick");
	if ( GetPrivLevel() >= pSrc->GetPrivLevel() )
	{
		pSrc->SysMessageDefault(DEFMSG_MSG_ACC_PRIV);
		return false;
	}

	if ( fBlock )
	{
		SetPrivFlags(PRIV_BLOCKED);
		pSrc->SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_MSG_ACC_BLOCK), GetName());
	}

	TCHAR *z = Str_GetTemp();
	sprintf(z, g_Cfg.GetDefaultMsg(DEFMSG_MSG_ACC_KICK), GetName(), fBlock ? "KICK" : "DISCONNECT", pSrc->GetName());
	g_Log.Event(LOGL_EVENT|LOGM_GM_CMDS, "%s\n", z);
	return true;
}

size_t CAccount::AttachChar(CChar *pChar)
{
	ADDTOCALLSTACK("CAccount::AttachChar");
	ASSERT(pChar);
	ASSERT(IsMyAccountChar(pChar));

	size_t i = m_Chars.AttachChar(pChar);
	if ( i != m_Chars.BadIndex() )
	{
		size_t iQty = m_Chars.GetCharCount();
		if ( iQty > MAX_CHARS_PER_ACCT )
			g_Log.Event(LOGM_ACCOUNTS|LOGL_ERROR, "Account '%s' exceeded max characters allowed (%" FMTSIZE_T "/%d)\n", GetName(), iQty, MAX_CHARS_PER_ACCT);
	}
	return i;
}

size_t CAccount::DetachChar(CChar *pChar)
{
	ADDTOCALLSTACK("CAccount::DetachChar");
	ASSERT(pChar);
	ASSERT(IsMyAccountChar(pChar));

	if ( m_uidLastChar == pChar->GetUID() )
		m_uidLastChar.InitUID();

	return m_Chars.DetachChar(pChar);
}

void CAccount::DeleteChars()
{
	ADDTOCALLSTACK("CAccount::DeleteChars");
	if ( g_Serv.IsLoading() )
		return;

	size_t i = m_Chars.GetCharCount();
	while ( i > 0 )
	{
		CChar *pChar = m_Chars.GetChar(--i).CharFind();
		if ( pChar )
			pChar->Delete();

		m_Chars.DetachChar(i);
	}
}

bool CAccount::IsMyAccountChar(const CChar *pChar) const
{
	ADDTOCALLSTACK("CAccount::IsMyAccountChar");
	if ( !pChar || !pChar->m_pPlayer )
		return false;
	return (pChar->m_pPlayer->m_pAccount == this);
}

CClient *CAccount::FindClient(const CClient *pExclude) const
{
	ADDTOCALLSTACK("CAccount::FindClient");

	CClient *pClient = NULL;
	ClientIterator it;
	for ( pClient = it.next(); pClient != NULL; pClient = it.next() )
	{
		if ( pClient == pExclude )
			continue;
		if ( pClient->m_pAccount == this )
			return pClient;
	}
	return NULL;
}

enum AC_TYPE
{
	AC_ACCOUNT,
	AC_BLOCK,
	AC_CHARS,
	AC_CHARUID,
	AC_CHATNAME,
	AC_FIRSTCONNECTDATE,
	AC_FIRSTIP,
	AC_GUEST,
	AC_JAIL,
	AC_LANG,
	AC_LASTCHARUID,
	AC_LASTCONNECTDATE,
	AC_LASTCONNECTTIME,
	AC_LASTIP,
	AC_MAXCHARS,
	AC_MD5PASSWORD,
	AC_NAME,
	AC_NEWPASSWORD,
	AC_PASSWORD,
	AC_PLEVEL,
	AC_PRIV,
	AC_RESDISP,
	AC_TAG,
	AC_TAG0,
	AC_TAGCOUNT,
	AC_TOTALCONNECTTIME,
	AC_QTY
};

LPCTSTR const CAccount::sm_szLoadKeys[AC_QTY + 1] =	// static
{
	"ACCOUNT",
	"BLOCK",
	"CHARS",
	"CHARUID",
	"CHATNAME",
	"FIRSTCONNECTDATE",
	"FIRSTIP",
	"GUEST",
	"JAIL",
	"LANG",
	"LASTCHARUID",
	"LASTCONNECTDATE",
	"LASTCONNECTTIME",
	"LASTIP",
	"MAXCHARS",
	"MD5PASSWORD",
	"NAME",
	"NEWPASSWORD",
	"PASSWORD",
	"PLEVEL",
	"PRIV",
	"RESDISP",
	"TAG",
	"TAG0",
	"TAGCOUNT",
	"TOTALCONNECTTIME",
	NULL,
};

bool CAccount::r_LoadVal(CScript &s)
{
	ADDTOCALLSTACK("CAccount::r_LoadVal");
	EXC_TRY("LoadVal");

	int index = FindTableHeadSorted(s.GetKey(), sm_szLoadKeys, COUNTOF(sm_szLoadKeys) - 1);
	if ( index < 0 )
		return false;

	switch ( static_cast<AC_TYPE>(index) )
	{
		case AC_BLOCK:
			if ( !s.HasArgs() || s.GetArgVal() )
				SetPrivFlags(PRIV_BLOCKED);
			else
				ClearPrivFlags(PRIV_BLOCKED);
			break;
		case AC_CHARUID:
			if ( !g_Serv.IsLoading() )
			{
				CGrayUID uid(s.GetArgVal());
				CChar *pChar = uid.CharFind();
				if ( !pChar )
				{
					DEBUG_ERR(("Invalid CHARUID 0%lx for account '%s'\n", static_cast<DWORD>(uid), GetName()));
					return false;
				}
				if ( !IsMyAccountChar(pChar) )
				{
					DEBUG_ERR(("CHARUID 0%lx (%s) not attached to account '%s'\n", static_cast<DWORD>(uid), pChar->GetName(), GetName()));
					return false;
				}
				AttachChar(pChar);
			}
			break;
		case AC_CHATNAME:
			m_sChatName = s.GetArgStr();
			break;
		case AC_FIRSTCONNECTDATE:
			m_dateFirstConnect.Read(s.GetArgStr());
			break;
		case AC_FIRSTIP:
			m_First_IP.SetAddrStr(s.GetArgStr());
			break;
		case AC_GUEST:
			if ( !s.HasArgs() || s.GetArgVal() )
				SetPrivLevel(PLEVEL_Guest);
			else if ( GetPrivLevel() == PLEVEL_Guest )
				SetPrivLevel(PLEVEL_Player);
			break;
		case AC_JAIL:
			if ( !s.HasArgs() || s.GetArgVal() )
				SetPrivFlags(PRIV_JAILED);
			else
				ClearPrivFlags(PRIV_JAILED);
			break;
		case AC_LANG:
			m_lang.Set(s.GetArgStr());
			break;
		case AC_LASTCHARUID:
			m_uidLastChar = static_cast<CGrayUID>(s.GetArgVal());
			break;
		case AC_LASTCONNECTDATE:
			m_dateLastConnect.Read(s.GetArgStr());
			break;
		case AC_LASTCONNECTTIME:
			m_Last_Connect_Time = s.GetArgLLVal();
			break;
		case AC_LASTIP:
			m_Last_IP.SetAddrStr(s.GetArgStr());
			break;
		case AC_MAXCHARS:
			SetMaxChars(static_cast<BYTE>(s.GetArgVal()));
			break;
		case AC_NEWPASSWORD:
			SetNewPassword(s.GetArgStr());
			break;
		case AC_PASSWORD:
			SetPassword(s.GetArgStr());
			break;
		case AC_MD5PASSWORD:
			SetPassword(s.GetArgStr(), true);
			break;
		case AC_PLEVEL:
			SetPrivLevel(GetPrivLevelText(s.GetArgRaw()));
			break;
		case AC_PRIV:
			m_PrivFlags = static_cast<WORD>(s.GetArgVal());
			if ( m_PrivFlags & PRIV_UNUSED )
			{
				g_Log.EventError("Fixing PRIV field (0%hx) for account %s have not supported flags set (caught by mask 0%hx)\n", m_PrivFlags, GetName(), static_cast<WORD>(PRIV_UNUSED));
				m_PrivFlags &= ~PRIV_UNUSED;
			}
			break;
		case AC_RESDISP:
			SetResDisp(static_cast<BYTE>(minimum(maximum(s.GetArgVal(), RDS_NONE), RDS_QTY - 1)));
			break;
		case AC_TAG0:
		{
			bool fQuoted = false;
			m_TagDefs.SetStr(s.GetKey() + 5, fQuoted, s.GetArgStr(&fQuoted), true);
			return true;
		}
		case AC_TAG:
		{
			bool fQuoted = false;
			m_TagDefs.SetStr(s.GetKey() + 4, fQuoted, s.GetArgStr(&fQuoted));
			return true;
		}
		case AC_TOTALCONNECTTIME:
			m_Total_Connect_Time = s.GetArgLLVal();
			break;
		default:
			return false;
	}
	return true;
	EXC_CATCH;

	EXC_DEBUG_START;
	EXC_ADD_SCRIPT;
	EXC_DEBUG_END;
	return false;
}

bool CAccount::r_WriteVal(LPCTSTR pszKey, CGString &sVal, CTextConsole *pSrc)
{
	ADDTOCALLSTACK("CAccount::r_WriteVal");
	EXC_TRY("WriteVal");
	if ( !pSrc )
		return false;

	bool fZero = false;
	switch ( static_cast<AC_TYPE>(FindTableHeadSorted(pszKey, sm_szLoadKeys, COUNTOF(sm_szLoadKeys) - 1)) )
	{
		case AC_ACCOUNT:
		case AC_NAME:
			sVal = m_sName;
			break;
		case AC_BLOCK:
			sVal.FormatVal(IsPriv(PRIV_BLOCKED));
			break;
		case AC_CHARS:
			sVal.FormatVal(m_Chars.GetCharCount());
			break;
		case AC_CHATNAME:
			sVal = m_sChatName;
			break;
		case AC_FIRSTCONNECTDATE:
			sVal = m_dateFirstConnect.Format(NULL);
			break;
		case AC_FIRSTIP:
			sVal = m_First_IP.GetAddrStr();
			break;
		case AC_GUEST:
			sVal.FormatVal(GetPrivLevel() == PLEVEL_Guest);
			break;
		case AC_JAIL:
			sVal.FormatVal(IsPriv(PRIV_JAILED));
			break;
		case AC_LANG:
			sVal = m_lang.GetStr();
			break;
		case AC_LASTCHARUID:
			sVal.FormatHex(m_uidLastChar);
			break;
		case AC_LASTCONNECTDATE:
			sVal = m_dateLastConnect.Format(NULL);
			break;
		case AC_LASTCONNECTTIME:
			sVal.FormatLLVal(m_Last_Connect_Time);
			break;
		case AC_LASTIP:
			sVal = m_Last_IP.GetAddrStr();
			break;
		case AC_MAXCHARS:
			sVal.FormatVal(GetMaxChars());
			break;
		case AC_NEWPASSWORD:
			if ( (pSrc->GetPrivLevel() < PLEVEL_Admin) || (pSrc->GetPrivLevel() < GetPrivLevel()) )
				return false;
			sVal = GetNewPassword();
			break;
		case AC_PASSWORD:
		case AC_MD5PASSWORD:
			if ( (pSrc->GetPrivLevel() < PLEVEL_Admin) || (pSrc->GetPrivLevel() < GetPrivLevel()) )
				return false;
			sVal = GetPassword();
			break;
		case AC_PLEVEL:
			sVal.FormatVal(m_PrivLevel);
			break;
		case AC_PRIV:
			sVal.FormatHex(m_PrivFlags);
			break;
		case AC_RESDISP:
			sVal.FormatVal(m_ResDisp);
			break;
		case AC_TAG0:
			fZero = true;
			pszKey++;
		case AC_TAG:
		{
			if ( pszKey[3] != '.' )
				return false;
			pszKey += 4;
			sVal = m_TagDefs.GetKeyStr(pszKey, fZero);
			return true;
		}
		case AC_TAGCOUNT:
			sVal.FormatVal(m_TagDefs.GetCount());
			break;
		case AC_TOTALCONNECTTIME:
			sVal.FormatLLVal(m_Total_Connect_Time);
			break;
		default:
			return CScriptObj::r_WriteVal(pszKey, sVal, pSrc);
	}
	return true;
	EXC_CATCH;

	EXC_DEBUG_START;
	EXC_ADD_KEYRET(pSrc);
	EXC_DEBUG_END;
	return false;
}

enum AV_TYPE
{
	AV_BLOCK,
	AV_DELETE,
	AV_KICK,
	AV_TAGLIST
};

LPCTSTR const CAccount::sm_szVerbKeys[] =
{
	"BLOCK",
	"DELETE",
	"KICK",
	"TAGLIST",
	NULL,
};

bool CAccount::r_Verb(CScript &s, CTextConsole *pSrc)
{
	ADDTOCALLSTACK("CAccount::r_Verb");
	EXC_TRY("Verb");
	ASSERT(pSrc);

	if ( (pSrc->GetPrivLevel() < GetPrivLevel()) && (pSrc->GetPrivLevel() < PLEVEL_Admin) )
		return false;

	LPCTSTR pszKey = s.GetKey();
	if ( !strnicmp(pszKey, "CLEARTAGS", 9) )
	{
		pszKey = s.GetArgStr();
		SKIP_SEPARATORS(pszKey);
		m_TagDefs.ClearKeys(pszKey);
		return true;
	}

	int index = FindTableSorted(s.GetKey(), sm_szVerbKeys, COUNTOF(sm_szVerbKeys) - 1);
	if ( index < 0 )
	{
		bool fLoad = CScriptObj::r_Verb(s, pSrc);
		if ( !fLoad )	// try calling custom functions
		{
			CGString sVal;
			CScriptTriggerArgs Args(s.GetArgRaw());
			fLoad = r_Call(pszKey, pSrc, &Args, &sVal);
		}
		return fLoad;
	}

	switch ( static_cast<AV_TYPE>(index) )
	{
		case AV_DELETE:
		{
			CClient *pClient = FindClient();
			if ( pClient )
			{
				pClient->CharDisconnect();
				pClient->m_NetState->markReadClosed();
			}

			LPCTSTR pszCurrentName = GetName();
			char *z = Str_GetTemp();
			if ( g_Accounts.Account_Delete(this) )
				sprintf(z, "Account '%s' deleted\n", pszCurrentName);
			else
				sprintf(z, "Account '%s' deletion blocked via script\n", pszCurrentName);

			g_Log.EventStr(0, z);
			if ( pSrc != &g_Serv )
				pSrc->SysMessage(z);
			return true;
		}
		case AV_BLOCK:
			if ( !s.HasArgs() || s.GetArgVal() )
				SetPrivFlags(PRIV_BLOCKED);
			else
				ClearPrivFlags(PRIV_BLOCKED);
			return true;
		case AV_KICK:
		{
			CClient *pClient = FindClient();
			if ( pClient )
				pClient->addKick(pSrc, (index == AV_BLOCK));
			else
				Kick(pSrc, (index == AV_BLOCK));
			return true;
		}
		case AV_TAGLIST:
			m_TagDefs.DumpKeys(pSrc, "TAG.");
			return true;
		default:
			break;
	}
	EXC_CATCH;

	EXC_DEBUG_START;
	EXC_ADD_SCRIPTSRC;
	EXC_DEBUG_END;
	return false;
}

bool CAccount::r_GetRef(LPCTSTR &pszKey, CScriptObj *&pRef)
{
	ADDTOCALLSTACK("CAccount::r_GetRef");
	if ( !strnicmp(pszKey, "CHAR.", 5) )
	{
		pszKey += 5;
		size_t i = Exp_GetVal(pszKey);
		if ( m_Chars.IsValidIndex(i) )
			pRef = m_Chars.GetChar(i).CharFind();

		SKIP_SEPARATORS(pszKey);
		return true;
	}
	return CScriptObj::r_GetRef(pszKey, pRef);
}

void CAccount::r_Write(CScript &s)
{
	ADDTOCALLSTACK_INTENSIVE("CAccount::r_Write");
	if ( GetPrivLevel() >= PLEVEL_QTY )
		return;

	s.WriteSection("%s", static_cast<LPCTSTR>(m_sName));

	if ( GetPrivLevel() != PLEVEL_Player )
		s.WriteKey("PLEVEL", sm_szPrivLevels[GetPrivLevel()]);
	if ( m_PrivFlags )
		s.WriteKeyHex("PRIV", m_PrivFlags & ~(PRIV_BLOCKED|PRIV_JAILED));
	if ( GetResDisp() )
		s.WriteKeyVal("RESDISP", GetResDisp());
	if ( m_MaxChars )
		s.WriteKeyVal("MAXCHARS", m_MaxChars);
	if ( IsPriv(PRIV_JAILED) )
		s.WriteKeyVal("JAIL", 1);
	if ( IsPriv(PRIV_BLOCKED) )
		s.WriteKeyVal("BLOCK", 1);
	if ( !m_sPassword.IsEmpty() )
		s.WriteKey("PASSWORD", GetPassword());
	if ( !m_sNewPassword.IsEmpty() )
		s.WriteKey("NEWPASSWORD", GetNewPassword());
	if ( m_Total_Connect_Time )
		s.WriteKeyVal("TOTALCONNECTTIME", m_Total_Connect_Time);
	if ( m_Last_Connect_Time )
		s.WriteKeyVal("LASTCONNECTTIME", m_Last_Connect_Time);
	if ( m_uidLastChar.IsValidUID() )
		s.WriteKeyHex("LASTCHARUID", m_uidLastChar);

	m_Chars.WritePartyChars(s);

	if ( m_dateFirstConnect.IsTimeValid() )
		s.WriteKey("FIRSTCONNECTDATE", m_dateFirstConnect.Format(NULL));
	if ( m_First_IP.IsValidAddr() )
		s.WriteKey("FIRSTIP", m_First_IP.GetAddrStr());

	if ( m_dateLastConnect.IsTimeValid() )
		s.WriteKey("LASTCONNECTDATE", m_dateLastConnect.Format(NULL));
	if ( m_Last_IP.IsValidAddr() )
		s.WriteKey("LASTIP", m_Last_IP.GetAddrStr());
	if ( !m_sChatName.IsEmpty() )
		s.WriteKey("CHATNAME", static_cast<LPCTSTR>(m_sChatName));
	if ( m_lang.IsDef() )
		s.WriteKey("LANG", m_lang.GetStr());

	m_BaseDefs.r_WritePrefix(s);	// new variable storage system
	m_TagDefs.r_WritePrefix(s, "TAG");
}
