#include "HL2EditorPrivatePCH.h"

#include "BSPBrushUtils.h"
#include "Engine/Polys.h"
#include "IHL2Runtime.h"

constexpr float snapThreshold = 1.0f / 4.0f;

FBSPBrushUtils::FBSPBrushUtils()
{
}

void FBSPBrushUtils::BuildBrushGeometry(const FBSPBrush& brush, FMeshDescription& meshDesc)
{
	const int sideNum = brush.Sides.Num();

	TMeshAttributesRef<FVertexID, FVector> vertexPosAttr = meshDesc.VertexAttributes().GetAttributesRef<FVector>(MeshAttribute::Vertex::Position);
	TMeshAttributesRef<FVertexInstanceID, FVector2D> vertexInstUVAttr = meshDesc.VertexInstanceAttributes().GetAttributesRef<FVector2D>(MeshAttribute::VertexInstance::TextureCoordinate);
	TMeshAttributesRef<FPolygonGroupID, FName> polyGroupMaterialAttr = meshDesc.PolygonGroupAttributes().GetAttributesRef<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
	TMeshAttributesRef<FEdgeID, bool> edgeIsHardAttr = meshDesc.EdgeAttributes().GetAttributesRef<bool>(MeshAttribute::Edge::IsHard);
	TMeshAttributesRef<FEdgeID, float> edgeCreaseSharpnessAttr = meshDesc.EdgeAttributes().GetAttributesRef<float>(MeshAttribute::Edge::CreaseSharpness);

	// Iterate all planes
	TMap<FPolygonID, int> polyToSideMap;
	polyToSideMap.Reserve(sideNum);
	for (int i = 0; i < sideNum; ++i)
	{
		const FBSPBrushSide& side = brush.Sides[i];
		if (!side.EmitGeometry) { continue; }

		// Check that the texture is aligned to the face - if not, discard the face
		const FVector textureNorm = FVector::CrossProduct(side.TextureU, side.TextureV).GetUnsafeNormal();
		if (FMath::Abs(FVector::DotProduct(textureNorm, side.Plane)) < 0.1f) { continue; }

		// Create a poly for this side
		FPoly poly = FPoly::BuildInfiniteFPoly(side.Plane);
		
		// Iterate all other sides
		int numVerts = 0;
		for (int j = 0; j < sideNum; ++j)
		{
			if (j != i)
			{
				const FBSPBrushSide& otherSide = brush.Sides[j];

				// Slice poly with it
				FVector normal = FVector(otherSide.Plane) * -1.0f;
				numVerts = poly.Split(normal, FVector::PointPlaneProject(FVector::ZeroVector, otherSide.Plane));
				if (numVerts < 3) { break; }
			}
		}

		// Check if we have a valid polygon
		if (numVerts < 3) { continue; }
		numVerts = poly.Fix();
		if (numVerts < 3) { continue; }

		// Get or create polygon group
		FPolygonGroupID polyGroupID;
		{
			bool found = false;
			for (const FPolygonGroupID& otherPolyGroupID : meshDesc.PolygonGroups().GetElementIDs())
			{
				if (polyGroupMaterialAttr[otherPolyGroupID] == side.Material)
				{
					found = true;
					polyGroupID = otherPolyGroupID;
					break;
				}
			}
			if (!found)
			{
				polyGroupID = meshDesc.CreatePolygonGroup();
				polyGroupMaterialAttr[polyGroupID] = side.Material;
			}
		}
		
		// Create vertices
		TArray<FVertexInstanceID> polyContour;
		TSet<FVertexID> visited;
		visited.Reserve(poly.Vertices.Num());
		/*for (FVector& pos : poly.Vertices)
		{
			SnapVertex(pos);
		}*/
		for (const FVector& pos : poly.Vertices)
		{
			// Get or create vertex
			FVertexID vertID;
			{
				bool found = false;
				for (const FVertexID& otherVertID : meshDesc.Vertices().GetElementIDs())
				{
					const FVector& otherPos = vertexPosAttr[otherVertID];
					if (otherPos.Equals(pos, snapThreshold))
					{
						found = true;
						vertID = otherVertID;

						break;
					}
				}
				if (!found)
				{
					vertID = meshDesc.CreateVertex();
					vertexPosAttr[vertID] = pos;
				}
			}
			if (visited.Contains(vertID)) { continue; }
			visited.Add(vertID);

			// Create vertex instance
			FVertexInstanceID vertInstID = meshDesc.CreateVertexInstance(vertID);
			polyContour.Add(vertInstID);

			// Calculate UV
			FVector vertPos = vertexPosAttr[vertID];
			vertexInstUVAttr.Set(vertInstID, 0, FVector2D(
				(FVector::DotProduct(side.TextureU, vertPos) + side.TextureU.W) / side.TextureW,
				(FVector::DotProduct(side.TextureV, vertPos) + side.TextureV.W) / side.TextureH
			));
			
		}

		// Create poly
		polyToSideMap.Add(meshDesc.CreatePolygon(polyGroupID, polyContour), i);
	}

	// Apply smoothing groups
	for (const auto& pair : polyToSideMap)
	{
		const FBSPBrushSide& side = brush.Sides[pair.Value];

		TArray<FEdgeID> edgeIDs;
		meshDesc.GetPolygonEdges(pair.Key, edgeIDs);
		for (const FEdgeID& edgeID : edgeIDs)
		{
			const TArray<FPolygonID>& connectedPolyIDs = meshDesc.GetEdgeConnectedPolygons(edgeID);
			uint32 accumSmoothingGroup = side.SmoothingGroups;
			for (const FPolygonID& otherPolyID : connectedPolyIDs)
			{
				if (otherPolyID != pair.Key)
				{
					int* otherSideIndex = polyToSideMap.Find(otherPolyID);
					if (otherSideIndex != nullptr)
					{
						const FBSPBrushSide& otherSide = brush.Sides[*otherSideIndex];
						accumSmoothingGroup &= otherSide.SmoothingGroups;
					}
				}
			}
			if (accumSmoothingGroup > 0)
			{
				edgeIsHardAttr[edgeID] = false;
				edgeCreaseSharpnessAttr[edgeID] = 0.0f;
			}
			else
			{
				edgeIsHardAttr[edgeID] = true;
				edgeCreaseSharpnessAttr[edgeID] = 1.0f;
			}
		}
	}
}

inline void FBSPBrushUtils::SnapVertex(FVector& vertex)
{
	vertex.X = FMath::GridSnap(vertex.X, snapThreshold);
	vertex.Y = FMath::GridSnap(vertex.Y, snapThreshold);
	vertex.Z = FMath::GridSnap(vertex.Z, snapThreshold);
}