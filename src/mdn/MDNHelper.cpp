//
// VMime library (http://www.vmime.org)
// Copyright (C) 2002-2005 Vincent Richard <vincent@vincent-richard.net>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of
// the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//

#include "vmime/mdn/MDNHelper.hpp"

#include "vmime/exception.hpp"
#include "vmime/stringContentHandler.hpp"


namespace vmime {
namespace mdn {


void MDNHelper::attachMDNRequest(ref <message> msg, const mailboxList& mailboxes)
{
	ref <header> hdr = msg->getHeader();

	hdr->DispositionNotificationTo()->setValue(mailboxes);
}


void MDNHelper::attachMDNRequest(ref <message> msg, const mailbox& mbox)
{
	mailboxList mboxList;
	mboxList.appendMailbox(mbox.clone().dynamicCast <mailbox>());

	attachMDNRequest(msg, mboxList);
}


const std::vector <sendableMDNInfos> MDNHelper::getPossibleMDNs(const ref <const message> msg)
{
	std::vector <sendableMDNInfos> result;

	const ref <const header> hdr = msg->getHeader();

	if (hdr->hasField(fields::DISPOSITION_NOTIFICATION_TO))
	{
		const mailboxList& dnto = hdr->DispositionNotificationTo()->getValue();

		for (int i = 0 ; i < dnto.getMailboxCount() ; ++i)
			result.push_back(sendableMDNInfos(msg, *dnto.getMailboxAt(i)));
	}

	return (result);
}


const bool MDNHelper::isMDN(const ref <const message> msg)
{
	const ref <const header> hdr = msg->getHeader();

	// A MDN message implies the following:
	//   - a Content-Type field is present and its value is "multipart/report"
	//   - a "report-type" parameter is present in the Content-Type field,
	//     and its value is "disposition-notification"
	if (hdr->hasField(fields::CONTENT_TYPE))
	{
		const contentTypeField& ctf = *(hdr->ContentType());

		if (ctf.getValue().getType() == vmime::mediaTypes::MULTIPART &&
		    ctf.getValue().getSubType() == vmime::mediaTypes::MULTIPART_REPORT)
		{
			if (ctf.hasParameter("report-type") &&
			    ctf.getReportType() == "disposition-notification")
			{
				return (true);
			}
		}
	}

	return (false);
}


receivedMDNInfos MDNHelper::getReceivedMDN(const ref <const message> msg)
{
	if (!isMDN(msg))
		throw exceptions::invalid_argument();

	return receivedMDNInfos(msg);
}


const bool MDNHelper::needConfirmation(const ref <const message> msg)
{
	ref <const header> hdr = msg->getHeader();

	// No "Return-Path" field
	if (!hdr->hasField(fields::RETURN_PATH))
		return (true);

	// More than one address in Disposition-Notification-To
	if (hdr->hasField(fields::DISPOSITION_NOTIFICATION_TO))
	{
		const mailboxList& dnto = hdr->DispositionNotificationTo()->getValue();

		if (dnto.getMailboxCount() > 1)
			return (true);

		// Return-Path != Disposition-Notification-To
		const mailbox& mbox = *dnto.getMailboxAt(0);
		const path& rp = hdr->ReturnPath()->getValue();

		if (mbox.getEmail() != rp.getLocalPart() + "@" + rp.getDomain())
			return (true);
	}

	// User confirmation not needed
	return (false);
}


ref <message> MDNHelper::buildMDN(const sendableMDNInfos& mdnInfos,
                                  const string& text,
                                  const charset& ch,
                                  const mailbox& expeditor,
                                  const disposition& dispo,
                                  const string& reportingUA,
                                  const std::vector <string>& reportingUAProducts)
{
	// Create a new message
	ref <message> msg = vmime::create <message>();

	// Fill-in header fields
	ref <header> hdr = msg->getHeader();

	hdr->ContentType()->setValue(mediaType(vmime::mediaTypes::MULTIPART,
	                                       vmime::mediaTypes::MULTIPART_REPORT));
	hdr->ContentType()->setReportType("disosition-notification");

	hdr->Disposition()->setValue(dispo);

	hdr->To()->getValue().appendAddress(vmime::create <mailbox>(mdnInfos.getRecipient()));
	hdr->From()->getValue() = expeditor;
	hdr->Subject()->getValue().appendWord(vmime::create <word>("Disposition notification"));

	hdr->Date()->setValue(datetime::now());
	hdr->MimeVersion()->setValue(string(SUPPORTED_MIME_VERSION));

	msg->getBody()->appendPart(createFirstMDNPart(mdnInfos, text, ch));
	msg->getBody()->appendPart(createSecondMDNPart(mdnInfos,
		dispo, reportingUA, reportingUAProducts));
	msg->getBody()->appendPart(createThirdMDNPart(mdnInfos));

	return (msg);
}


ref <bodyPart> MDNHelper::createFirstMDNPart(const sendableMDNInfos& /* mdnInfos */,
                                             const string& text, const charset& ch)
{
	ref <bodyPart> part = vmime::create <bodyPart>();

	// Header
	ref <header> hdr = part->getHeader();

	hdr->ContentType()->setValue(mediaType(vmime::mediaTypes::TEXT,
	                                       vmime::mediaTypes::TEXT_PLAIN));

	hdr->ContentType()->setCharset(ch);

	// Body
	part->getBody()->setContents(vmime::create <stringContentHandler>(text));

	return (part);
}


ref <bodyPart> MDNHelper::createSecondMDNPart(const sendableMDNInfos& mdnInfos,
                                              const disposition& dispo,
                                              const string& reportingUA,
                                              const std::vector <string>& reportingUAProducts)
{
	ref <bodyPart> part = vmime::create <bodyPart>();

	// Header
	ref <header> hdr = part->getHeader();

	hdr->ContentDisposition()->setValue(vmime::contentDispositionTypes::INLINE);
	hdr->ContentType()->setValue(mediaType(vmime::mediaTypes::MESSAGE,
	                                       vmime::mediaTypes::MESSAGE_DISPOSITION_NOTIFICATION));

	// Body
	//
	//   The body of a message/disposition-notification consists of one or
	//   more "fields" formatted according to the ABNF of [RFC-MSGFMT] header
	//   "fields".  The syntax of the message/disposition-notification content
	//   is as follows:
	//
	//      disposition-notification-content = [ reporting-ua-field CRLF ]
	//      [ mdn-gateway-field CRLF ]
	//      [ original-recipient-field CRLF ]
	//      final-recipient-field CRLF
	//      [ original-message-id-field CRLF ]
	//      disposition-field CRLF
	//      *( failure-field CRLF )
	//      *( error-field CRLF )
	//      *( warning-field CRLF )
	//      *( extension-field CRLF )
	//
	header fields;

	// -- Reporting-UA (optional)
	if (!reportingUA.empty())
	{
		string ruaText;
		ruaText = reportingUA;

		for (unsigned int i = 0 ; i < reportingUAProducts.size() ; ++i)
		{
			if (i == 0)
				ruaText += "; ";
			else
				ruaText += ", ";

			ruaText += reportingUAProducts[i];
		}

		ref <defaultField> rua =
			(headerFieldFactory::getInstance()->create
				(vmime::fields::REPORTING_UA)).dynamicCast <defaultField>();

		rua->setValue(ruaText);

		fields.appendField(rua);
	}

	// -- Final-Recipient
	ref <defaultField> fr =
		(headerFieldFactory::getInstance()->
			create(vmime::fields::FINAL_RECIPIENT)).dynamicCast <defaultField>();

	fr->setValue("rfc822; " + mdnInfos.getRecipient().getEmail());

	// -- Original-Message-ID
	if (mdnInfos.getMessage()->getHeader()->hasField(vmime::fields::MESSAGE_ID))
	{
		fields.OriginalMessageId()->setValue
			(mdnInfos.getMessage()->getHeader()->MessageId()->getValue());
	}

	// -- Disposition
	fields.Disposition()->setValue(dispo);


	std::ostringstream oss;
	utility::outputStreamAdapter vos(oss);

	fields.generate(vos);

	part->getBody()->setContents(vmime::create <stringContentHandler>(oss.str()));

	return (part);
}


ref <bodyPart> MDNHelper::createThirdMDNPart(const sendableMDNInfos& mdnInfos)
{
	ref <bodyPart> part = vmime::create <bodyPart>();

	// Header
	ref <header> hdr = part->getHeader();

	hdr->ContentDisposition()->setValue(vmime::contentDispositionTypes::INLINE);
	hdr->ContentType()->setValue(mediaType(vmime::mediaTypes::TEXT,
	                                       vmime::mediaTypes::TEXT_RFC822_HEADERS));

	// Body: original message headers
	std::ostringstream oss;
	utility::outputStreamAdapter vos(oss);

	mdnInfos.getMessage()->getHeader()->generate(vos);

	part->getBody()->setContents(vmime::create <stringContentHandler>(oss.str()));

	return (part);
}


} // mdn
} // vmime
