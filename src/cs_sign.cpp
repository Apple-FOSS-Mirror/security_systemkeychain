/*
 * Copyright (c) 2006-2007 Apple Inc. All Rights Reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

//
// cs_sign - codesign signing operation
//
#include "codesign.h"
#include <Security/Security.h>
#include <Security/SecCodeSigner.h>
#include <Security/SecRequirementPriv.h>
#include <Security/CSCommonPriv.h>
#include <security_utilities/blob.h>
#include <cstdio>
#include <cmath>

using namespace UnixPlusPlus;


//
// One-time preparation
//
static CFMutableDictionaryRef parameters;		// common signing parameters
static SecCodeSignerRef signerRef;				// global signer object

void prepareToSign()
{
	parameters = makeCFMutableDictionary();
	SecCSFlags flags = kSecCSDefaultFlags;

	if (signer)
		CFDictionaryAddValue(parameters,
			kSecCodeSignerIdentity, signer);
	else
		flags |= kSecCSRemoveSignature;
	
	if (uniqueIdentifier)
		CFDictionaryAddValue(parameters,
			kSecCodeSignerIdentifier, CFTempString(uniqueIdentifier));
	if (identifierPrefix)
		CFDictionaryAddValue(parameters,
			kSecCodeSignerIdentifierPrefix, CFTempString(identifierPrefix));
	
	if (internalReq)
		CFDictionaryAddValue(parameters,
			kSecCodeSignerRequirements, readRequirements(internalReq));
	
	if (signatureSize)
		CFDictionaryAddValue(parameters, CFSTR("cmssize"), CFTempNumber(signatureSize));
	if (pagesize != pagesizeUnspecified)
		CFDictionaryAddValue(parameters, kSecCodeSignerPageSize, CFTempNumber(pagesize));
	if (cdFlags)
		CFDictionaryAddValue(parameters, kSecCodeSignerFlags, CFTempNumber(cdFlags));
	if (signingTime)
		CFDictionaryAddValue(parameters, kSecCodeSignerSigningTime, signingTime);
		
	if (detached)
		CFDictionaryAddValue(parameters, kSecCodeSignerDetached, CFTempURL(detached));
	else if (detachedDb)
		CFDictionaryAddValue(parameters, kSecCodeSignerDetached, kCFNull);
	
	if (resourceRules) {
		if (CFRef<CFDataRef> data = cfLoadFile(resourceRules)) {
			CFDictionaryAddValue(parameters, kSecCodeSignerResourceRules,
				CFRef<CFDictionaryRef>(makeCFDictionaryFrom(data)));
		} else
			fail("%s: cannot read resources", resourceRules);
	}
	
	if (entitlements) {
		if (CFRef<CFDataRef> data = cfLoadFile(entitlements)) {	// load the proposed entitlement blob
			if (CFRef<CFDictionaryRef> dict = makeCFDictionaryFrom(data)) {
				// plain plist - (silently) wrap into canonical blob form
				BlobWrapper *wrap = BlobWrapper::alloc(CFDataGetBytePtr(data), CFDataGetLength(data), kSecCodeMagicEntitlement);
				CFDictionaryAddValue(parameters, kSecCodeSignerEntitlements, CFTempData(*(BlobCore*)wrap));
				::free(wrap);
			} else {
				const BlobCore *blob = reinterpret_cast<const BlobCore *>(CFDataGetBytePtr(data));
				if (blob->magic() != kSecCodeMagicEntitlement)
					note(0, "%s: unrecognized blob type (accepting blindly)", entitlements);
				if (blob->length() != CFDataGetLength(data))
					fail("%s: invalid length in entitlement blob", entitlements);
				CFDictionaryAddValue(parameters, kSecCodeSignerEntitlements, CFTempData(*blob));
			}
		} else
			fail("%s: cannot read entitlement data", entitlements);
	}
	
	if (dryrun)
		CFDictionaryAddValue(parameters, kSecCodeSignerDryRun, kCFBooleanTrue);
	
	MacOSError::check(SecCodeSignerCreate(parameters, flags, &signerRef));
}


//
// Sign a code object.
//
void sign(const char *target)
{
	secdebug("codesign", "BEGIN SIGNING %s", target);
	
	CFRef<SecStaticCodeRef> code;
	MacOSError::check(SecStaticCodeCreateWithPath(CFTempURL(target), kSecCSDefaultFlags,
		&code.aref()));
	
	CFRef<CFDictionaryRef> dict;
	switch (OSStatus rc = SecCodeCopySigningInformation(code,
		preserveMetadata ? SecCSFlags(kSecCSRequirementInformation) : kSecCSDefaultFlags,
		&dict.aref())) {
	case noErr:
		if (CFDictionaryGetValue(dict, kSecCodeInfoIdentifier)) {	// binary is signed
			if (detached || detachedDb)
				note(0, "%s: not disturbing embedded signature", target);
			else if (force)
				note(0, "%s: replacing existing signature", target);
			else if (signer)
				fail("%s: is already signed", target);
		}
		break;
	case errSecCSSignatureFailed:	// signed but signature invalid
	case CSSMERR_TP_NOT_TRUSTED:	// cert chain invalid
		if (detached)
			note(0, "%s: ignoring invalid embedded signature", target);
		else if (force)
			note(0, "%s: replacing invalid existing signature", target);
		else if (signer)
			fail("%s: is already signed", target);
		break;
	default:
		MacOSError::throwMe(rc);
	}
	
	CFCopyRef<SecCodeSignerRef> currentSigner = signerRef;		// the one we prepared during setup
	if (preserveMetadata) {
		CFRef<CFMutableDictionaryRef> param = CFDictionaryCreateMutableCopy(NULL, 0, parameters);
		if (dict && !CFDictionaryGetValue(param, kSecCodeSignerRequirements))
			if (CFTypeRef ireqs = CFDictionaryGetValue(dict, kSecCodeInfoRequirementData))
				CFDictionaryAddValue(param, kSecCodeSignerRequirements, ireqs);
		if (dict && !CFDictionaryGetValue(param, kSecCodeSignerEntitlements))
			if (CFTypeRef entitlements = CFDictionaryGetValue(dict, kSecCodeInfoEntitlements))
				CFDictionaryAddValue(param, kSecCodeSignerEntitlements, entitlements);
		MacOSError::check(SecCodeSignerCreate(param, kSecCSDefaultFlags, &currentSigner.aref()));
	}
	
	ErrorCheck check;
	check(SecCodeSignerAddSignatureWithErrors(currentSigner, code, kSecCSDefaultFlags, check));

	SecCSFlags flags = kSecCSDefaultFlags;
	if (modifiedFiles)
		flags |= kSecCSContentInformation;
	MacOSError::check(SecCodeCopySigningInformation(code, flags, &dict.aref()));
	note(1, "%s: signed %s [%s]", target,
		cfString(CFStringRef(CFDictionaryGetValue(dict, kSecCodeInfoFormat))).c_str(),
		cfString(CFStringRef(CFDictionaryGetValue(dict, kSecCodeInfoIdentifier))).c_str()
	);
	if (modifiedFiles)
		writeFileList(CFArrayRef(CFDictionaryGetValue(dict, kSecCodeInfoChangedFiles)), modifiedFiles, "a");
}
