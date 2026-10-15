#pragma once

#include <string>
#include <string_view>

namespace aether
{
	// Session identity is plain display text, bounded without splitting a UTF-8 character.
	inline std::string NormalizeSessionName(std::string_view name)
	{
		std::string result;
		for (const unsigned char byte : name)
		{
			const char character = byte < 32 || byte == 127 ? ' ' : static_cast<char>(byte);
			if (character == ' ' && (result.empty() || result.back() == ' '))
			{
				continue;
			}
			result.push_back(character);
		}
		if (result.size() > 64)
		{
			std::size_t end = 64;
			while (end > 0 && (static_cast<unsigned char>(result[end]) & 0xc0u) == 0x80u)
			{
				--end;
			}
			result.resize(end);
		}
		while (!result.empty() && result.back() == ' ')
		{
			result.pop_back();
		}
		return result;
	}

	inline std::string ComposeSessionWindowTitle(std::string_view base, std::string_view session)
	{
		return session.empty() ? std::string(base) : std::string(base) + " [session: " + std::string(session) + "]";
	}
}
