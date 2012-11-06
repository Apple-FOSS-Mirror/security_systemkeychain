/*
 * Copyright (c) 2000-2001 Apple Computer, Inc. All Rights Reserved.
 * 
 * The contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License. Please obtain
 * a copy of the License at http://www.apple.com/publicsource and read it before
 * using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
 * OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
 * specific language governing rights and limitations under the License.
 */


//
// systemkeychain command - set up and manipulate system-unlocked keychains
//
#include <security_cdsa_client/securestorage.h>
#include <security_cdsa_client/cryptoclient.h>
#include <security_cdsa_utilities/uniformrandom.h>
#include <security_utilities/devrandom.h>
#include <security_utilities/errors.h>
#include <security_cdsa_client/wrapkey.h>
#include <security_cdsa_client/genkey.h>
#include <security_cdsa_utilities/Schema.h>
#include <security_cdsa_utilities/cssmendian.h>
#include <securityd_client/ssblob.h>
#include <securityd_client/ssclient.h>
#include <Security/cssmapple.h>
#include <cstdarg>
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacErrors.h>
#include "TokenIDHelper.h"

using namespace SecurityServer;
using namespace CssmClient;
using namespace UnixPlusPlus;


static const char *unlockConfig = kSystemUnlockFile;


//
// Values set from command-line options
//
const char *systemKCName = kSystemKeychainDir kSystemKeychainName;
bool verbose = false;
bool createIfNeeded = false;
bool force = false;


//
// CSSM record attribute names
//
static const CSSM_DB_ATTRIBUTE_INFO dlInfoLabel = {
	CSSM_DB_ATTRIBUTE_NAME_AS_STRING,
	{"Label"},
	CSSM_DB_ATTRIBUTE_FORMAT_BLOB
};



//
// Local functions
void usage();
void createSystemKeychain(const char *kcName, const char *passphrase);
void extract(const char *srcName, const char *dstName);
void test(const char *kcName);

void notice(const char *fmt, ...);
void fail(const char *fmt, ...);

void labelForMasterKey(Db &db, CssmOwnedData &data);
void deleteKey(Db &db, const CssmData &label);	// delete key with this label

void addReferralRecord(Db &srcDb, const DLDbIdentifier &ident, CssmData *refData, CFDataRef labelData);
void createUnlockConfig(CSP &csp, Db &db);
void createAKeychain(const char *kcName, const char *passphrase, CSP &csp, DL &dl, Db &db, Key *masterKeyRef);
OSStatus createTokenProtectedKeychain(const char *kcName);
void createMasterKey(CSP &csp, Key &masterKeyRef);
void flattenKey  (const CssmKey &rawKey, CssmDataContainer &flatKey);

//
// Main program: parse options and dispatch, catching exceptions
//
int main (int argc, char * argv[])
{
	enum Action {
		showUsage,
		setupSystem,
		copyKey,
		testUnlock,
		tokenProtectedKCCreate
	} action = showUsage;

	extern int optind;
	extern char *optarg;
	int arg;
	OSStatus status;
	while ((arg = getopt(argc, argv, "cCfk:stvT:")) != -1) {
		switch (arg) {
		case 'c':
			createIfNeeded = true;
			break;
		case 'C':
			action = setupSystem;
			break;
		case 'f':
			force = true;
			break;
		case 'k':
			systemKCName = optarg;
			break;
		case 's':
			action = copyKey;
			break;
		case 't':
			action = testUnlock;
			break;
		case 'v':
			verbose = true;
			break;
		case 'T':					// Create token protected keychain
			action = tokenProtectedKCCreate;
			systemKCName = optarg;
			break;
		default:
			usage();
		}
	}
	try {
		switch (action) {
		case setupSystem:
			if (optind < argc - 1)
				usage();
			createSystemKeychain(systemKCName, argv[optind]);
			break;
		case copyKey:
			if (optind == argc)
				usage();
			do {
				extract(argv[optind], systemKCName);
			} while (argv[++optind]);
			break;
		case testUnlock:
			test(systemKCName);
			break;
		case tokenProtectedKCCreate:
			if (status = createTokenProtectedKeychain(systemKCName))
			{
				cssmPerror("unable to create token protected keychain", status);
				exit(1);
			}
			break;
		default:
			usage();
		}
		exit(0);
	} catch (const CssmError &error) {
		cssmPerror(systemKCName, error.error);
		exit(1);
	} catch (const UnixError &error) {
		fail("%s: %s", systemKCName, strerror(error.error));
		exit(1);
	} catch (const MacOSError &error) {
		cssmPerror(systemKCName, error.error);
		exit(1);
	} 
	catch (...) {
		fail("Unexpected exception");
		exit(1);
	}
}


//
// Partial usage message (some features aren't worth emphasizing...)
//
void usage()
{
	fprintf(stderr, "Usage: systemkeychain -C [passphrase]  # (re)create system root keychain"
		"\n\tsystemkeychain [-k destination-keychain] -s source-keychain ..."
		"\n\tsystemkeychain -T token-protected-keychain-name"
		"\n");
	exit(2);
}


//
// Create a keychain and set it up as the system-root secret
//
void createSystemKeychain(const char *kcName, const char *passphrase)
{
	// for the default path only, make sure the directory exists
	if (!strcmp(kcName, kSystemKeychainDir kSystemKeychainName))
		::mkdir(kSystemKeychainDir, 0755);
	
	CSP csp(gGuidAppleCSPDL);
	DL dl(gGuidAppleCSPDL);
	
	// create the keychain, using appropriate credentials
	Db db(dl, kcName);
	Key masterKey;

	createMasterKey(csp, masterKey);
	createAKeychain(kcName, passphrase, csp, dl, db, &masterKey);
	createUnlockConfig(csp, db);
	
	notice("%s installed as system keychain", kcName);
}

void createAKeychain(const char *kcName, const char *passphrase, CSP &csp, DL &dl, Db &db, Key *masterKeyRef)
{
	// create the keychain, using appropriate credentials
	Allocator &alloc = db->allocator();
	AutoCredentials cred(alloc);	// will leak, but we're quitting soon :-)
	CSSM_CSP_HANDLE cspHandle = csp->handle();
	if (passphrase) {
		// use this passphrase
		cred += TypedList(alloc, CSSM_SAMPLE_TYPE_KEYCHAIN_CHANGE_LOCK,
			new(alloc) ListElement(CSSM_SAMPLE_TYPE_PASSWORD),
			new(alloc) ListElement(StringData(passphrase)));
		cred += TypedList(alloc, CSSM_SAMPLE_TYPE_KEYCHAIN_LOCK,
			new(alloc) ListElement(CSSM_SAMPLE_TYPE_PASSWORD),
			new(alloc) ListElement(StringData(passphrase)));
		db->accessCredentials(&cred);
	}
	else
	{
		// generate a random key
		notice("warning: this keychain cannot be unlocked with any passphrase");
		if (!masterKeyRef)				// caller does not need it returned for later use
			MacOSError::throwMe(paramErr);
		cred += TypedList(alloc, CSSM_SAMPLE_TYPE_KEYCHAIN_CHANGE_LOCK,
			new(alloc) ListElement(CSSM_SAMPLE_TYPE_SYMMETRIC_KEY),
			new(alloc) ListElement(CssmData::wrap(cspHandle)),
			new(alloc) ListElement(CssmData::wrap(static_cast<const CssmKey &>(*masterKeyRef))));
		cred += TypedList(alloc, CSSM_SAMPLE_TYPE_KEYCHAIN_LOCK,
			new(alloc) ListElement(CSSM_SAMPLE_TYPE_SYMMETRIC_KEY),
			new(alloc) ListElement(CssmData::wrap(cspHandle)),
			new(alloc) ListElement(CssmData::wrap(static_cast<const CssmKey &>(*masterKeyRef))),
			new(alloc) ListElement(CssmData()));
		db->accessCredentials(&cred);
	}
	db->dbInfo(&KeychainCore::Schema::DBInfo); // Set the standard schema
	try {
		db->create();
	} catch (const CssmError &error) {
		if (error.error == CSSMERR_DL_DATASTORE_ALREADY_EXISTS && force) {
			notice("recreating %s", kcName);
			unlink(kcName);
			db->create();
		} else
			throw;
	}
	chmod(db->name(), 0644);	
}

void createMasterKey(CSP &csp, Key &masterKeyRef)
{
	// generate a random key
	CssmClient::GenerateKey generate(csp, CSSM_ALGID_3DES_3KEY_EDE, 64 * 3);
	masterKeyRef =	generate(KeySpec(CSSM_KEYUSE_ANY, CSSM_KEYATTR_RETURN_REF | CSSM_KEYATTR_EXTRACTABLE));
}

void createUnlockConfig(CSP &csp, Db &db)
{
	// extract the key into the CSPDL
	DeriveKey derive(csp, CSSM_ALGID_KEYCHAIN_KEY, CSSM_ALGID_3DES_3KEY, 3 * 64);
	CSSM_DL_DB_HANDLE dlDb = db->handle();
	CssmData dlDbData = CssmData::wrap(dlDb);
	CssmKey refKey;
	KeySpec spec(CSSM_KEYUSE_ANY,
		CSSM_KEYATTR_RETURN_REF | CSSM_KEYATTR_EXTRACTABLE);
	derive(&dlDbData, spec, refKey);
	
	// now extract the raw keybits
	CssmKey rawKey;
	WrapKey wrap(csp, CSSM_ALGID_NONE);
	wrap(refKey, rawKey);

	// form the evidence record
	UnlockBlob blob;
	blob.initialize(0);
	
	CssmAutoData dbBlobData(Allocator::standard());
	SecurityServer::ClientSession client(dbBlobData.allocator, dbBlobData.allocator);
	db->copyBlob(dbBlobData.get());
	DbBlob *dbBlob = dbBlobData.get().interpretedAs<DbBlob>();
	
	memcpy(&blob.signature, &dbBlob->randomSignature, sizeof(dbBlob->randomSignature));
	memcpy(blob.masterKey, rawKey.data(), sizeof(blob.masterKey));

	// write it out, forcibly overwriting an existing file
	string tempFile(string(unlockConfig) + ",");
	FileDesc blobFile(tempFile, O_WRONLY | O_CREAT | O_TRUNC, 0400);
	if (blobFile.write(blob) != sizeof(blob)) {
		unlink(tempFile.c_str());
		fail("unable to write %s", tempFile.c_str());
	}
	blobFile.close();
	::rename(tempFile.c_str(), unlockConfig);
}

//
// Extract the master secret from a keychain and install it in another keychain for unlocking
//
void extract(const char *srcName, const char *dstName)
{
	using namespace KeychainCore;

	CSPDL cspdl(gGuidAppleCSPDL);
	
	// open source database
	Db srcDb(cspdl, srcName);
	
	// open destination database
	Db dstDb(cspdl, dstName);
	try {
		dstDb->open();
	} catch (const CssmError &err) {
		if (err.error == CSSMERR_DL_DATASTORE_DOESNOT_EXIST && createIfNeeded) {
			notice("creating %s", dstName);
			dstDb->create();
		} else
			throw;
	}
	
	// extract master key and place into destination keychain
	DeriveKey derive(cspdl, CSSM_ALGID_KEYCHAIN_KEY, CSSM_ALGID_3DES_3KEY, 3 * 64);
	CSSM_DL_DB_HANDLE dstDlDb = dstDb->handle();
	derive.add(CSSM_ATTRIBUTE_DL_DB_HANDLE, dstDlDb);
	CSSM_DL_DB_HANDLE srcDlDb = srcDb->handle();
	CssmData dlDbData = CssmData::wrap(srcDlDb);
	CssmAutoData keyLabel(Allocator::standard());
	labelForMasterKey(srcDb, keyLabel);
	KeySpec spec(CSSM_KEYUSE_ANY,
		CSSM_KEYATTR_RETURN_REF | CSSM_KEYATTR_PERMANENT | CSSM_KEYATTR_SENSITIVE,
		keyLabel);
	CssmKey masterKey;
	try {
		derive(&dlDbData, spec, masterKey);
	} catch (const CssmError &error) {
		if (error.error != CSSMERR_DL_INVALID_UNIQUE_INDEX_DATA)
			throw;
		if (!force)
			fail("existing key in %s not overwritten. Use -f to replace it.", dstDb->name());
		notice("replacing existing record in %s", dstDb->name());
		deleteKey(dstDb, keyLabel);
		derive(&dlDbData, spec, masterKey);
	}
	
	addReferralRecord(srcDb, dstDb->dlDbIdentifier(), NULL, NULL);
	
	notice("%s can now be unlocked with a key in %s", srcName, dstName);
}

void addReferralRecord(Db &srcDb, const DLDbIdentifier &ident, CssmData *refData, CFDataRef label)
{
	// If you have a Db dstDb, call with dstDb->dlDbIdentifier() as second parameter
	using namespace KeychainCore;
	// now add a referral record to the source database
	uint32 referralType = (!refData)?CSSM_APPLE_UNLOCK_TYPE_KEY_DIRECT:CSSM_APPLE_UNLOCK_TYPE_WRAPPED_PRIVATE;
	try {
		// build attribute vector
		CssmAutoData masterKeyLabel(Allocator::standard());
		CssmData externalLabel;
		if (label)
		{
			externalLabel.Data = const_cast<uint8 *>(CFDataGetBytePtr(label));
			externalLabel.Length = CFDataGetLength(label);
		}
		else
			labelForMasterKey(srcDb, masterKeyLabel);
		CssmAutoDbRecordAttributeData refAttrs(9);
		refAttrs.add(Schema::kUnlockReferralType, uint32(referralType));
		refAttrs.add(Schema::kUnlockReferralDbName, ident.dbName());
		refAttrs.add(Schema::kUnlockReferralDbNetname, CssmData());
		refAttrs.add(Schema::kUnlockReferralDbGuid, ident.ssuid().guid());
		refAttrs.add(Schema::kUnlockReferralDbSSID, ident.ssuid().subserviceId());
		refAttrs.add(Schema::kUnlockReferralDbSSType, ident.ssuid().subserviceType());
		refAttrs.add(Schema::kUnlockReferralKeyLabel, label?externalLabel:masterKeyLabel.get());
		refAttrs.add(Schema::kUnlockReferralKeyAppTag, CssmData());
		refAttrs.add(Schema::kUnlockReferralPrintName,
			StringData("Keychain Unlock Referral Record"));

		// no reference data for this form
		CssmData emptyRefData;
		if (!refData)
			refData = &emptyRefData;
			
		try {
			srcDb->insert(CSSM_DL_DB_RECORD_UNLOCK_REFERRAL,
				&refAttrs,
				refData);
			secdebug("kcreferral", "referral record stored in %s", srcDb->name());
		} catch (const CssmError &e) {
			if (e.error != CSSMERR_DL_INVALID_RECORDTYPE)
				throw;

			// Create the referral relation and retry
			secdebug("kcreferral", "adding referral schema relation to %s", srcDb->name());
			srcDb->createRelation(CSSM_DL_DB_RECORD_UNLOCK_REFERRAL, "CSSM_DL_DB_RECORD_UNLOCK_REFERRAL",
				Schema::UnlockReferralSchemaAttributeCount,
				Schema::UnlockReferralSchemaAttributeList,
				Schema::UnlockReferralSchemaIndexCount,
				Schema::UnlockReferralSchemaIndexList);
			srcDb->insert(CSSM_DL_DB_RECORD_UNLOCK_REFERRAL,
				&refAttrs,
				refData);
			secdebug("kcreferral", "referral record inserted in %s (on retry)", srcDb->name());
		}
	} catch (...) {
		notice("kcreferral", "cannot store referral in %s", srcDb->name());
		throw;
	}
}

//
// Create a keychain protected with an asymmetric key stored on a token (e.g. smartcard)
// See createSystemKeychain for a similar call
//
#include <Security/SecKey.h>
#include <Security/SecKeychain.h>
#include <Security/SecKeychainItem.h>

OSStatus createTokenProtectedKeychain(const char *kcName)
{
	// In the notation of "extract", srcDb is the keychain to be unlocked
	// (e.g. login keychain), and dstDb is the one to unlock with (e.g. smartcard/token)
	// @@@ A later enhancement should allow lookup by public key hash
	
	SecKeychainRef keychainRef = NULL;
	SecKeyRef publicKeyRef = NULL;
	const CSSM_KEY *pcssmKey;
	CssmKey pubEncryptionKey;
	CssmKey nullWrappedPubEncryptionKey;
	CFDataRef label = NULL;
	OSStatus status = findFirstEncryptionPublicKeyOnToken(&publicKeyRef, &keychainRef, &label);
	if (status)
		return status;
	
	status = SecKeyGetCSSMKey(publicKeyRef,  &pcssmKey);
	if (status)
		return status;
	pubEncryptionKey = *pcssmKey;

	CSSM_DL_DB_HANDLE dstdldbHandle;
	status = SecKeychainGetDLDBHandle(keychainRef, &dstdldbHandle);
	if (status)
		return status;

	char *tokenName;
	CSSM_SUBSERVICE_UID tokenUID;

	CSSM_RETURN cx;
	cx = CSSM_DL_GetDbNameFromHandle (dstdldbHandle, &tokenName);
	if (cx)
		return cx;

	cx = CSSM_GetSubserviceUIDFromHandle (dstdldbHandle.DLHandle, &tokenUID);
	if (cx)
		return cx;

	secdebug("kcreferral", "Key found on token with subserviceId: %d", tokenUID.SubserviceId);
	DLDbIdentifier tokenIdentifier(tokenName, Guid(gGuidAppleSdCSPDL), tokenUID.SubserviceId, 
			CSSM_SERVICE_CSP | CSSM_SERVICE_DL , NULL);

	CSP csp(gGuidAppleCSPDL);
	DL dl(gGuidAppleCSPDL);
	
	// create the keychain, using appropriate credentials
	Db srcdb(dl, kcName);
	Key masterKey;

	createMasterKey(csp, masterKey);
	createAKeychain(kcName, NULL, csp, dl, srcdb, &masterKey);

	// Turn the raw key into a reference key in the local csp with a NULL unwrap
	CssmClient::Key publicEncryptionKey(csp, pubEncryptionKey, true);

	UnwrapKey tunwrap(csp, CSSM_ALGID_NONE);
	CSSM_KEYUSE uu = CSSM_KEYUSE_ANY;
	CSSM_KEYATTR_FLAGS attrs = CSSM_KEYATTR_RETURN_REF | CSSM_KEYATTR_EXTRACTABLE;
	tunwrap(publicEncryptionKey, KeySpec(uu, attrs), nullWrappedPubEncryptionKey);
	CssmClient::Key nullWrappedPubEncryptionKeyX(csp, nullWrappedPubEncryptionKey, true);

	// use a CMS wrap to encrypt the key
	CssmKey wrappedKey;
	WrapKey wrap(csp, CSSM_ALGID_RSA);		// >>>> will be key on token -- WrapKey wrap
	wrap.key(nullWrappedPubEncryptionKeyX);
	wrap.mode(CSSM_ALGMODE_NONE);
	wrap.padding(CSSM_PADDING_PKCS1);
	// only add if not CSSM_KEYBLOB_WRAPPED_FORMAT_NONE
	wrap.add(CSSM_ATTRIBUTE_WRAPPED_KEY_FORMAT, uint32(CSSM_KEYBLOB_WRAPPED_FORMAT_PKCS7));
	wrap(masterKey, wrappedKey);

	CssmDataContainer flatKey;
	flattenKey(wrappedKey, flatKey);
	addReferralRecord(srcdb, tokenIdentifier, &flatKey, label);

	if (publicKeyRef)
		CFRelease(publicKeyRef);
	if (label)
		CFRelease(label);
		
	return noErr;
}

void flattenKey(const CssmKey &rawKey, CssmDataContainer &flatKey)
{
	// Flatten the raw input key naively: key header then key data
	// We also convert it to network byte order in case the referral
	// record ends up being used on a different machine
	// A CSSM_KEY is a CSSM_KEYHEADER followed by a CSSM_DATA
	
	CssmKey rawKeyCopy(rawKey);
	const uint32 keyDataLength = rawKeyCopy.length();
	uint32 sz = (sizeof(CSSM_KEY) + keyDataLength);
	flatKey.Data = flatKey.mAllocator.alloc<uint8>(sz);
	flatKey.Length = sz;
	
	Security::h2ni(rawKeyCopy.KeyHeader);	// convert it to network byte order
	
	// Now copy: header, then key struct, then key data
	memcpy(flatKey.Data, &rawKeyCopy.header(), sizeof(CSSM_KEYHEADER));
	memcpy(flatKey.Data + sizeof(CSSM_KEYHEADER), &rawKeyCopy.keyData(), sizeof(CSSM_DATA));
	memcpy(flatKey.Data + sizeof(CSSM_KEYHEADER) + sizeof(CSSM_DATA), rawKeyCopy.data(), keyDataLength);
	// Note that the Data pointer in the CSSM_DATA portion will not be meaningful when unpacked later
	// We will also fill in the unflattened key length based on the size of flatKey
}

//
// Run a simple test to see if the system-root keychain can auto-unlock.
// This isn't trying really hard to diagnose any problems; it's just a yay-or-nay check.
//
void test(const char *kcName)
{
	CSP csp(gGuidAppleCSPDL);
	DL dl(gGuidAppleCSPDL);
	
	// lock, then unlock the keychain
	Db db(dl, kcName);
	printf("Testing system unlock of %s\n", kcName);
	printf("(If you are prompted for a passphrase, cancel)\n");
	try {
		db->lock();
		db->unlock();
		notice("System unlock is working");
	} catch (...) {
		fail("System unlock is NOT working\n");
	}
}


//
// Utility functions
//
void labelForMasterKey(Db &db, CssmOwnedData &label)
{
	// create a random signature
	char signature[8];
	UniformRandomBlobs<DevRandomGenerator>().random(signature);
	
	// concatenate prefix string with random signature
	label = StringData("*UNLOCK*");	// 8 bytes exactly
	label.append(signature, sizeof(signature));
	assert(label.length() == 8 + sizeof(signature));
}


void deleteKey(Db &db, const CssmData &label)
{
	DbCursor search(db);
	search->recordType(CSSM_DL_DB_RECORD_SYMMETRIC_KEY);
	search->add(CSSM_DB_EQUAL, dlInfoLabel, label);
	DbUniqueRecord id;
	if (search->next(NULL, NULL, id))
		id->deleteRecord();
}


//
// Message helpers
//
void notice(const char *fmt, ...)
{
	if (verbose) {
		va_list args;
		va_start(args, fmt);
		vprintf(fmt, args);
		putchar('\n');
		va_end(args);
	}
}

void fail(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	putchar('\n');
	va_end(args);
	exit(1);
}
