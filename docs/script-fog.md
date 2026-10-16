# Analytic fog from managed scripts

`Renderer.FogParameters` reads `(density, heightFalloff, sunScatter, maxOpacity)`.
`Renderer.SetFog(density, heightFalloff, sunScatter, maxOpacity)` writes those existing renderer parameters. Non-finite writes are ignored; the first three values are clamped nonnegative, opacity to [0,1]. Calls use the game-thread script export context, like the lighting and sky setters.

This uses the existing analytic height integral in `HeightFog.slangh`: each shaded surface evaluates its camera-to-world distance and height. It does not enable volumetric marching, add a full-screen pass, or alter culling, collision or asset uploads. Fog colour follows the existing authored sky; this API adds no separate fog colour override.

Scripts should save the previous parameters on attach and restore them on detach. Sky components may overwrite renderer fog on their next update; scenes with an authored Sky should drive that component instead of competing with it. Volumetric sky settings also take precedence over analytic surface fog. A high exponential height falloff can remove mist in high-altitude caves; choose a small falloff for near-uniform underground haze.

Validated in the consuming game with a native terrain descent/return and no script errors. Editor, GameRuntime and EngineTests builds passed. Repeat XML unit run: 1,352 passing, zero failures. The initial gauntlet unit run reported one failure that did not reproduce; the script retained its failed aggregate result but did not retain the individual failing case. Strict editor/runtime smokes reached ready with zero validation errors, but remained red for the previously observed startup VK_SUBOPTIMAL_KHR and enabled-layer advisory classification. This is not a fully green gauntlet.
