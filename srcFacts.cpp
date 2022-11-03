/*
    srcFacts.cpp

    Produces a report with various measures of source code.
    Supports C++, C, Java, and C#.

    Input is an XML file in the srcML format.

    Output is a markdown table with the measures.

    Output performance statistics to stderr.

    Code includes an embedded XML parser:
    * No checking for well-formedness
    * No DTD declarations
*/

#include <iostream>
#include <locale>
#include <iterator>
#include <string>
#include <algorithm>
#include <sys/types.h>
#include <errno.h>
#include <string_view>
#include <optional>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdlib.h>
#include <bitset>

#if !defined(_MSC_VER)
#include <sys/uio.h>
#include <unistd.h>
#define READ read
#else
#include <BaseTsd.h>
#include <io.h>
typedef SSIZE_T ssize_t;
#define READ _read
#endif

// provides literal string operator""sv
using namespace std::literals::string_view_literals;

const int BUFFER_SIZE = 16 * 16 * 4096;

const std::bitset<128> tagNameMask("00000111111111111111111111111110100001111111111111111111111111100000001111111111011000000000000000000000000000000000000000000000");

constexpr auto TAG_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";

constexpr auto SPACE_CHARS = " \n\t\r\v\f"sv;

/*
    Refill the buffer preserving the unused data.
    Current content is shifted left and new data
    appended to the rest of the contents.

    @param[in, out] buffer Container for characters
    @param[in, out] contents View of buffer
    @return Number of bytes read
    @retval 0 EOF
    @retval -1 Read error
*/
int refillBuffer(std::string& buffer, std::string_view& contents) {

    // move unprocessed characters in contents to start of the buffer
    std::copy(contents.cbegin(), contents.cend(), buffer.begin());

    // read in whole blocks
    ssize_t readBytes = 0;
    while (((readBytes = READ(0, static_cast<void*>(buffer.data() + contents.size()),
        buffer.size() - contents.size())) == -1) && (errno == EINTR)) {
    }
    if (readBytes == -1)
        // error in read
        return -1;
    if (readBytes == 0) {
        // EOF
        contents = ""sv;
        return 0;
    }

    // set contents to the start of the buffer
    contents = std::string_view(&buffer[0], contents.size() + readBytes);

    return readBytes;
}

// trace parsing
#ifdef TRACE
#undef TRACE
#define HEADER(m) std::clog << std::setw(10) << std::left << m <<"\t"
#define FIELD(l, n) l << ":|" << n << "| "
#define FIELD2(l1, n1, l2, n2) FIELD(l1,n1) << FIELD(l2,n2)
#define TRACE0(m)
#define TRACE1(m, l1, n1) HEADER(m) << FIELD(l1,n1) << '\n';
#define TRACE2(m, l1, n1, l2, n2) HEADER(m) << FIELD(l1,n1) << FIELD(l2,n2) << '\n';
#define TRACE3(m, l1, n1, l2, n2, l3, n3) HEADER(m) << FIELD(l1,n1) << FIELD(l2,n2) << FIELD(l3,n3) << '\n';
#define TRACE4(m, l1, n1, l2, n2, l3, n3, l4, n4) HEADER(m) << FIELD(l1,n1) << FIELD(l2,n2) << FIELD(l3,n3) << FIELD(l4,n4) << '\n';
#define GET_TRACE(_1,_2,_3,_4,_5,_6,_7,_8,_9,NAME,...) NAME
#define TRACE(...) GET_TRACE(__VA_ARGS__, TRACE4, _UNUSED, TRACE3, _UNUSED, TRACE2, _UNUSED, TRACE1, _UNUSED, TRACE0)(__VA_ARGS__)
#else
#define TRACE(...)
#endif

int main() {
    const auto start = std::chrono::steady_clock::now();
    std::string url;
    int textsize = 0;
    int loc = 0;
    int exprCount = 0;
    int functionCount = 0;
    int classCount = 0;
    int unitCount = 0;
    int declCount = 0;
    int commentCount = 0;
    int depth = 0;
    long totalBytes = 0;
    bool inTag = false;
    bool inXMLComment = false;
    bool inCDATA = false;
    std::string inTagQName;
    std::string_view inTagPrefix;
    std::string_view inTagLocalName;
    bool isArchive = false;
    std::string buffer(BUFFER_SIZE, ' ');
    std::string_view contents;
    TRACE("START DOCUMENT");
    while (true) {
        if (contents.size() < 5) {
            // refill buffer and adjust iterator
            int bytesRead = refillBuffer(buffer, contents);
            if (bytesRead < 0) {
                std::cerr << "parser error : File input error\n";
                return 1;
            }
            totalBytes += bytesRead;
            if (!inXMLComment && !inCDATA && contents.empty())
                break;
        } else if (inTag && contents[0] == 'x' && contents[1] == 'm' && contents[2] == 'l' && contents[3] == 'n' && contents[4] == 's' && (contents[5] == ':' || contents[5] == '=')) {
            // parse XML namespace
            contents.remove_prefix(5);
            int nameEndPosition = contents.find('=');
            if (nameEndPosition == contents.npos) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            int prefixSize = 0;
            if (contents.front() == ':') {
                contents.remove_prefix(1);
                --nameEndPosition;
                prefixSize = nameEndPosition;
            }
            const std::string_view prefix(contents.substr(0, prefixSize));
            contents.remove_prefix(nameEndPosition + 1);
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            if (contents.empty()) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            const char delimiter = contents.front();
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            contents.remove_prefix(1);
            int valueEndPosition = contents.find(delimiter);
            if (valueEndPosition == contents.npos) {
                std::cerr << "parser error : incomplete namespace\n";
                return 1;
            }
            const std::string_view uri(contents.substr(0, valueEndPosition));
            TRACE("NAMESPACE", "prefix", prefix, "uri", uri);
            contents.remove_prefix(valueEndPosition + 1);
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            if (contents[0] == '>') {
                contents.remove_prefix(1);
                inTag = false;
                ++depth;
            } else if (contents[0] == '/' && contents[1] == '>') {
                contents.remove_prefix(2);
                TRACE("END TAG", "prefix", inTagPrefix, "qName", inTagQName, "localName", inTagLocalName);
                inTag = false;
            }
        } else if (inTag) {
            // parse attribute
            int nameEndPosition = std::distance(&contents[0], std::find_if_not(contents.cbegin(), contents.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPosition == contents.size()) {
                std::cerr << "parser error : Empty attribute name" << '\n';
                return 1;
            }
            const std::string_view qName(contents.substr(0, nameEndPosition));
            size_t colonPosition = qName.find(':');
            if (colonPosition == 0) {
                std::cerr << "parser error : Invalid attribute name " << qName << '\n';
                return 1;
            }
            if (colonPosition == qName.npos)
                colonPosition = 0;
            const std::string_view prefix(qName.substr(0, colonPosition));
            const std::string_view localName(qName.substr(colonPosition ? colonPosition + 1 : 0));
            contents.remove_prefix(nameEndPosition);
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            if (contents.empty()) {
                std::cerr << "parser error : attribute " << qName << " incomplete attribute\n";
                return 1;
            }
            if (contents.front() != '=') {
                std::cerr << "parser error : attribute " << qName << " missing =\n";
                return 1;
            }
            contents.remove_prefix(1);
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            const char delimiter = contents.front();
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error : attribute " << qName << " missing delimiter\n";
                return 1;
            }
            contents.remove_prefix(1);
            int valueEndPosition = contents.find(delimiter); //std::distance(&contents[0], std::find_if_not(contents.cbegin(), contents.cend(), [] (char c) { return tagNameMask[c]; }));
            if (valueEndPosition == contents.size()) {
                std::cerr << "parser error : attribute " << qName << " missing delimiter\n";
                return 1;
            }
            const std::string_view value(contents.substr(0, valueEndPosition));
            if (localName == "url"sv)
                url = value;
            TRACE("ATTRIBUTE", "prefix", prefix, "qname", qName, "localName", localName, "value", value);
            contents.remove_prefix(valueEndPosition + 1);
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            if (contents[0] == '>') {
                contents.remove_prefix(1);
                inTag = false;
                ++depth;
            } else if (contents[0] == '/' && contents[1] == '>') {
                contents.remove_prefix(2);
                TRACE("END TAG", "prefix", inTagPrefix, "qName", inTagQName, "localName", inTagLocalName);
                inTag = false;
            }
        } else if (inXMLComment || contents.compare("<!==") == 0) {
        // } else if (inXMLComment || (contents[1] == '!' && contents[0] == '<' && contents[2] == '-' && contents[3] == '-')) {
            // parse XML comment
            if (contents.empty()) {
                std::cerr << "parser error : Unterminated XML comment\n";
                return 1;
            }
            if (!inXMLComment) {
                contents.remove_prefix(4);
            }
            constexpr std::string_view endComment = "-->"sv;
            int tagEndPosition = contents.find(endComment);
            inXMLComment = tagEndPosition == contents.size();
            const std::string_view comment(contents.substr(0, tagEndPosition));
            TRACE("COMMENT", "comment", comment);
            if (!inXMLComment) {
                contents.remove_prefix(tagEndPosition + 1 + endComment.size());
            } else {
                contents.remove_prefix(tagEndPosition);
            }
        } else if (inCDATA || (contents[1] == '!' && contents[0] == '<' && contents[2] == '[' && contents[3] == 'C' && contents[4] == 'D'
            && contents[5] == 'A' && contents[6] == 'T' && contents[7] == 'A' && contents[8] == '[')) {
            // parse CDATA
            if (contents.empty()) {
                std::cerr << "parser error : Unterminated CDATA\n";
                return 1;
            }
            constexpr std::string_view endCDATA = "]]>"sv;
            if (!inCDATA) {
                contents.remove_prefix(9);
            }
            int tagEndPosition = contents.find(endCDATA);
            inCDATA = tagEndPosition == contents.size();
            const std::string_view characters(contents.substr(0, tagEndPosition));
            TRACE("CDATA", "characters", characters);
            textsize += static_cast<int>(characters.size());
            loc += static_cast<int>(std::count(characters.cbegin(), characters.cend(), '\n'));
            if (!inCDATA) {
                contents.remove_prefix(tagEndPosition + endCDATA.size() + 1);
            } else {
                contents.remove_prefix(tagEndPosition);
            }

        } else if (contents[1] == '?' && contents[0] == '<' && contents[1] == '?' && contents[2] == 'x' && contents[3] == 'm' && contents[4] == 'l' && contents[5] == ' ') {
            // parse XML declaration
            constexpr std::string_view startXMLDecl = "<?xml";
            constexpr std::string_view endXMLDecl = "?>";
            auto tagEndPosition = contents.find('>');
            if (tagEndPosition == contents.npos) {
                int bytesRead = refillBuffer(buffer, contents);
                if (bytesRead < 0) {
                    std::cerr << "parser error : File input error\n";
                    return 1;
                }
                totalBytes += bytesRead;
                if ((tagEndPosition = contents.find('>')) == contents.npos) {
                    std::cerr << "parser error: Incomplete XML declaration\n";
                    return 1;
                }
            }
            contents.remove_prefix(startXMLDecl.size());
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            // parse required version
            // if (cursor == tagEnd) {
            //     std::cerr << "parser error: Missing space after before version in XML declaration\n";
            //     return 1;
            // }
            // std::string::const_iterator nameEnd = std::find(cursor, tagEnd, '=');
            auto nameEndPosition = contents.find('=');
            const std::string_view attr(contents.substr(0, nameEndPosition));
            contents.remove_prefix(nameEndPosition + 1);
            const char delimiter = contents.front();
            if (delimiter != '"' && delimiter != '\'') {
                std::cerr << "parser error: Invalid start delimiter for version in XML declaration\n";
                return 1;
            }
            contents.remove_prefix(1);
            int valueEndPosition = contents.find(delimiter);
            if (valueEndPosition == contents.npos) {
                std::cerr << "parser error: Invalid end delimiter for version in XML declaration\n";
                return 1;
            }
            if (attr != "version"sv) {
                std::cerr << "parser error: Missing required first attribute version in XML declaration\n";
                return 1;
            }
            const std::string_view version(contents.substr(0, valueEndPosition));
            contents.remove_prefix(valueEndPosition + 1);
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            // parse optional encoding and standalone attributes
            std::optional<std::string_view> encoding;
            std::optional<std::string_view> standalone;
            // FIX
            if (true) { //cursor != (tagEnd - 1)) {
                // nameEnd = std::find(cursor, tagEnd, '=');
                auto nameEndPosition = contents.find('=');
                // if (nameEnd == tagEnd) {
                //     std::cerr << "parser error: Incomplete attribute in XML declaration\n";
                //     return 1;
                // }
                const std::string_view attr2(contents.substr(0, nameEndPosition));
                contents.remove_prefix(nameEndPosition + 1);
                char delimiter2 = contents.front();
                if (delimiter2 != '"' && delimiter2 != '\'') {
                    std::cerr << "parser error: Invalid end delimiter for attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(1);
                // auto valueEnd = std::find(cursor, tagEnd, delimiter2);
                auto valueEndPosition = contents.find(delimiter2);
                // if (valueEnd == tagEnd) {
                //     std::cerr << "parser error: Incomplete attribute " << attr2 << " in XML declaration\n";
                //     return 1;
                // }
                if (attr2 == "encoding"sv) {
                    encoding = std::string_view(contents.substr(0, valueEndPosition));
                } else if (attr2 == "standalone"sv) {
                    standalone = std::string_view(contents.substr(0, valueEndPosition));
                } else {
                    std::cerr << "parser error: Invalid attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(valueEndPosition + 1);
                contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            }
            // FIX
            if (true) { // cursor != (tagEnd - endXMLDecl.size() + 1)) {
                // nameEnd = std::find(cursor, tagEnd, '=');
                auto nameEndPosition = contents.substr(0, tagEndPosition).find('=');
                if (nameEndPosition == contents.npos) {
                    std::cerr << "parser error: Incomplete attribute in XML declaration\n";
                    return 1;
                }
                const std::string_view attr2(contents.substr(0, nameEndPosition));
                contents.remove_prefix(nameEndPosition + 1);
                const char delimiter2 = contents.front();
                if (delimiter2 != '"' && delimiter2 != '\'') {
                    std::cerr << "parser error: Invalid end delimiter for attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(1);
                // auto valueEnd = std::find(cursor, tagEnd, delimiter2);
                auto valueEndPosition = contents.substr(0, tagEndPosition).find(delimiter2);
                if (valueEndPosition == contents.npos) {
                    std::cerr << "parser error: Incomplete attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                if (!standalone && attr2 == "standalone"sv) {
                    standalone = std::string_view(contents.substr(0, valueEndPosition));
                } else {
                    std::cerr << "parser error: Invalid attribute " << attr2 << " in XML declaration\n";
                    return 1;
                }
                contents.remove_prefix(valueEndPosition + 1);
                contents.remove_prefix(contents.substr(0, tagEndPosition).find_first_not_of(SPACE_CHARS));
            }
            TRACE("XML DECLARATION", "version", version, "encoding", (encoding ? *encoding : ""), "standalone", (standalone ? *standalone : ""));
            contents.remove_prefix(endXMLDecl.size());
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));

        } else if (contents[1] == '?' && contents[0] == '<') {
            // parse processing instruction
            int tagEndPosition = contents.find("?>"sv);
            if (tagEndPosition == contents.npos) {
                int bytesRead = refillBuffer(buffer, contents);
                if (bytesRead < 0) {
                    std::cerr << "parser error : File input error\n";
                    return 1;
                }
                totalBytes += bytesRead;
                tagEndPosition = contents.find("?>"sv);
                if (tagEndPosition == contents.npos) {
                    std::cerr << "parser error: Incomplete XML declaration\n";
                    return 1;
                }
            }
            contents.remove_prefix(2);
            int nameEndPosition = std::distance(&contents[0], std::find_if_not(contents.cbegin(), contents.cend(), [] (char c) { return tagNameMask[c]; }));
            // FIX
            if (nameEndPosition == 0) {
                std::cerr << "parser error : Unterminated processing instruction '" << contents.substr(0, nameEndPosition) << "'\n";
                return 1;
            }
            const std::string_view target(contents.substr(0, nameEndPosition));
            contents.remove_prefix(nameEndPosition);
            const std::string_view data(contents.substr(0, tagEndPosition));
            TRACE("PI", "target", target, "data", data);
            contents.remove_prefix(tagEndPosition);
            contents.remove_prefix(2);
        } else if (contents[1] == '/' && contents[0] == '<') {
            // parse end tag
            if (contents.size() < 100) {
                if (std::none_of(contents.cbegin(), contents.cend(), [] (char c) { return c =='>'; })) {
                    int bytesRead = refillBuffer(buffer, contents);
                    if (bytesRead < 0) {
                        std::cerr << "parser error : File input error\n";
                        return 1;
                    }
                    totalBytes += bytesRead;
                    // @TODO start search after initial sed
                    if (std::none_of(contents.cbegin(), contents.cend(), [] (char c) { return c =='>'; })) {
                        std::cerr << "parser error: Incomplete element end tag\n";
                        return 1;
                    }
                }
            }
            contents.remove_prefix(2);
            if (contents.front() == ':') {
                std::cerr << "parser error : Invalid end tag name\n";
                return 1;
            }
            int nameEndPosition = std::distance(&contents[0], std::find_if_not(contents.cbegin(), contents.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPosition == contents.size()) {
                std::cerr << "parser error : Unterminated end tag '" << contents.substr(0, nameEndPosition) << "'\n";
                return 1;
            }
            const std::string_view qName(contents.substr(0, nameEndPosition));
            if (qName.empty()) {
                std::cerr << "parser error: EndTag: invalid element name\n";
                return 1;
            }
            size_t colonPosition = qName.find(':');
            if (colonPosition == qName.npos) {
                colonPosition = 0;
            }
            const std::string_view prefix(qName.substr(0, colonPosition));
            const std::string_view localName(qName.substr(colonPosition + 1));
            contents.remove_prefix(nameEndPosition + 1);
            --depth;
            TRACE("END TAG", "prefix", prefix, "qName", qName, "localName", localName);
        } else if (contents.front() == '<') {
            // parse start tag
            if (contents.size() < 200) {
                if (std::none_of(contents.cbegin(), contents.cend(), [] (char c) { return c =='>'; })) {
                    int bytesRead = refillBuffer(buffer, contents);
                    if (bytesRead < 0) {
                        std::cerr << "parser error : File input error\n";
                        return 1;
                    }
                    totalBytes += bytesRead;
                    // @TODO start search after initial sed
                    if (std::none_of(contents.cbegin(), contents.cend(), [] (char c) { return c =='>'; })) {
                        std::cerr << "parser error: Incomplete element start tag\n";
                        return 1;
                    }
                }
            }
            contents.remove_prefix(1);
            if (contents.front() == ':') {
                std::cerr << "parser error : Invalid start tag name\n";
                return 1;
            }
            int nameEndPosition = std::distance(&contents[0], std::find_if_not(contents.cbegin(), contents.cend(), [] (char c) { return tagNameMask[c]; }));
            if (nameEndPosition == contents.size()) {
                std::cerr << "parser error : Unterminated start tag '" << contents.substr(0, nameEndPosition) << "'\n";
                return 1;
            }
            size_t colonPosition = 0;
            if (contents[nameEndPosition] == ':') {
                colonPosition = nameEndPosition;
                nameEndPosition = std::distance(&contents[0], std::find_if_not(contents.cbegin() + nameEndPosition + 1, contents.cend(), [] (char c) { return tagNameMask[c]; }));
            }
            const std::string_view qName(contents.substr(0, nameEndPosition));
            if (qName.empty()) {
                std::cerr << "parser error: StartTag: invalid element name\n";
                return 1;
            }
            const std::string_view prefix(qName.substr(0, colonPosition));
            const std::string_view localName(qName.substr(colonPosition ? colonPosition + 1 : 0, nameEndPosition));
            TRACE("START TAG", "prefix", prefix, "qName", qName, "localName", localName);
            if (localName == "expr"sv) {
                ++exprCount;
            } else if (localName == "decl"sv) {
                ++declCount;
            } else if (localName == "comment"sv) {
                ++commentCount;
            } else if (localName == "function"sv) {
                ++functionCount;
            } else if (localName == "unit"sv) {
                ++unitCount;
                if (depth == 1)
                    isArchive = true;
            } else if (localName == "class"sv) {
                ++classCount;
            }
            contents.remove_prefix(nameEndPosition);
            if (contents.front() != '>') {
                contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
            }
            if (contents.front() == '>') {
                contents.remove_prefix(1);
                ++depth;
            } else if (contents[0] == '/' && contents[1] == '>') {
                contents.remove_prefix(2);
                TRACE("END TAG", "prefix", prefix, "qName", qName, "localName", localName);
            } else {
                inTagQName = qName;
                inTagPrefix = inTagQName.substr(0, prefix.size());
                inTagLocalName = inTagQName.substr(prefix.size() + 1);
                inTag = true;
            }
        } else if (depth == 0) {
            // parse characters before or after XML
            contents.remove_prefix(contents.find_first_not_of(SPACE_CHARS));
        } else if (contents.front() == '&') {
            // parse character entity references
            std::string_view reference;
            if (contents[1] == 'l' && contents[2] == 't' && contents[3] == ';') {
                reference = "<";
                contents.remove_prefix(4);
            } else if (contents[1] == 'g' && contents[2] == 't' && contents[3] == ';') {
                reference = ">";
                contents.remove_prefix(4);
            } else if (contents[1] == 'a' && contents[2] == 'm' && contents[3] == 'p' && contents[4] == ';') {
                reference = "&";
                contents.remove_prefix(5);
            } else {
                reference = "&";
                contents.remove_prefix(1);
            }
            const std::string_view characters(reference);
            TRACE("ENTITYREF", "characters", characters);
            ++textsize;

        } else {
            // parse character non-entity references
            int tagEndPosition = contents.find_first_of("<&");
            const std::string_view characters(contents.substr(0, tagEndPosition));
            TRACE("CHARACTERS", "characters", characters);
            loc += static_cast<int>(std::count(characters.cbegin(), characters.cend(), '\n'));
            textsize += static_cast<int>(characters.size());
            contents.remove_prefix(characters.size());
        }
    }
    TRACE("END DOCUMENT");
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double> >(finish - start).count();
    const double mlocPerSec = loc / elapsed_seconds / 1000000;
    int files = unitCount;
    if (isArchive)
        --files;
    std::cout.imbue(std::locale{""});
    int valueWidth = std::max(5, static_cast<int>(log10(totalBytes) * 1.3 + 1));
    std::cout << "# srcFacts: " << url << '\n';
    std::cout << "| Measure      | " << std::setw(valueWidth + 3) << "Value |\n";
    std::cout << "|:-------------|-" << std::setw(valueWidth + 3) << std::setfill('-') << ":|\n" << std::setfill(' ');
    std::cout << "| srcML bytes  | " << std::setw(valueWidth) << totalBytes          << " |\n";
    std::cout << "| Characters   | " << std::setw(valueWidth) << textsize       << " |\n";
    std::cout << "| Files        | " << std::setw(valueWidth) << files          << " |\n";
    std::cout << "| LOC          | " << std::setw(valueWidth) << loc            << " |\n";
    std::cout << "| Classes      | " << std::setw(valueWidth) << classCount    << " |\n";
    std::cout << "| Functions    | " << std::setw(valueWidth) << functionCount << " |\n";
    std::cout << "| Declarations | " << std::setw(valueWidth) << declCount     << " |\n";
    std::cout << "| Expressions  | " << std::setw(valueWidth) << exprCount     << " |\n";
    std::cout << "| Comments     | " << std::setw(valueWidth) << commentCount  << " |\n";
    std::clog << '\n';
    std::clog << std::setprecision(3) << elapsed_seconds << " sec\n";
    std::clog << std::setprecision(3) << mlocPerSec << " MLOC/sec\n";
    return 0;
}
